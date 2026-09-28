//! An encrypted Nearby connection: offline frames through the secure channel, keep-alives answered and sent, and bytes
//! payloads reassembled from their chunks.

use std::collections::HashMap;
use std::time::Duration;

use prost::Message as _;
use tokio::net::TcpStream;
use tokio::net::tcp::OwnedWriteHalf;
use tokio::sync::mpsc;
use tokio::task::JoinHandle;

use crate::error::{Error, Result};
use crate::frame;
use crate::secure::Channel;
use crate::ukey2::Keys;
use crate::wire::connections::payload_transfer_frame::payload_chunk::Flags;
use crate::wire::connections::payload_transfer_frame::payload_header::PayloadType;
use crate::wire::connections::payload_transfer_frame::{PacketType, PayloadChunk, PayloadHeader};
use crate::wire::connections::{
    DisconnectionFrame, KeepAliveFrame, OfflineFrame, PayloadTransferFrame, V1Frame, offline_frame, v1_frame,
};
use crate::wire::sharing;

/// Android sends one every 10 s and gives up after about 30 s of silence.
const KEEP_ALIVE: Duration = Duration::from_secs(10);
const SILENCE: Duration = Duration::from_secs(30);
/// Bytes payloads carry setup frames and shared text; files come as file payloads.
const MAX_BYTES_PAYLOAD: i64 = 1024 * 1024;
pub const CHUNK: usize = 512 * 1024;

pub enum Incoming {
    Bytes { id: i64, data: Vec<u8> },
    FileChunk { id: i64, total: i64, offset: i64, body: Vec<u8>, last: bool },
    Disconnected,
}

pub struct Connection {
    writer: OwnedWriteHalf,
    frames: mpsc::Receiver<Result<Vec<u8>>>,
    reader: JoinHandle<()>,
    channel: Channel,
    assembling: HashMap<i64, Vec<u8>>,
}

impl Drop for Connection {
    fn drop(&mut self) {
        self.reader.abort();
    }
}

pub fn offline(kind: v1_frame::FrameType, fill: impl FnOnce(&mut V1Frame)) -> OfflineFrame {
    let mut v1 = V1Frame { r#type: Some(kind as i32), ..V1Frame::default() };
    fill(&mut v1);
    OfflineFrame { version: Some(offline_frame::Version::V1 as i32), v1: Some(v1) }
}

pub fn sharing_frame(kind: sharing::v1_frame::FrameType, fill: impl FnOnce(&mut sharing::V1Frame)) -> sharing::Frame {
    let mut v1 = sharing::V1Frame { r#type: Some(kind as i32), ..sharing::V1Frame::default() };
    fill(&mut v1);
    sharing::Frame { version: Some(sharing::frame::Version::V1 as i32), v1: Some(v1) }
}

impl Connection {
    pub fn new(stream: TcpStream, keys: &Keys) -> Self {
        let (mut read, writer) = stream.into_split();
        let (tx, frames) = mpsc::channel(4);
        // Reads run in their own task so waiting for a frame can be raced against the keep-alive timer.
        let reader = tokio::spawn(async move {
            loop {
                let next = frame::read(&mut read).await;
                let failed = next.is_err();
                if tx.send(next).await.is_err() || failed {
                    return;
                }
            }
        });
        Self { writer, frames, reader, channel: Channel::new(keys), assembling: HashMap::new() }
    }

    pub async fn send(&mut self, frame: &OfflineFrame) -> Result<()> {
        let sealed = self.channel.seal(&frame.encode_to_vec())?;
        frame::write(&mut self.writer, &sealed).await
    }

    pub async fn send_sharing(&mut self, frame: &sharing::Frame) -> Result<()> {
        let id = random_id()?;
        self.send_bytes(id, &frame.encode_to_vec()).await
    }

    /// A bytes payload goes as one chunk with its data and a second, empty one flagged last.
    pub async fn send_bytes(&mut self, id: i64, data: &[u8]) -> Result<()> {
        let total = i64::try_from(data.len()).map_err(|_| Error::Protocol("payload too large"))?;
        self.send_chunk(id, PayloadType::Bytes, total, 0, data.to_vec(), false).await?;
        self.send_chunk(id, PayloadType::Bytes, total, total, Vec::new(), true).await
    }

    pub async fn send_chunk(
        &mut self,
        id: i64,
        kind: PayloadType,
        total: i64,
        offset: i64,
        body: Vec<u8>,
        last: bool,
    ) -> Result<()> {
        let transfer = PayloadTransferFrame {
            packet_type: Some(PacketType::Data as i32),
            payload_header: Some(PayloadHeader {
                id: Some(id),
                r#type: Some(kind as i32),
                total_size: Some(total),
                is_sensitive: Some(false),
                ..PayloadHeader::default()
            }),
            payload_chunk: Some(PayloadChunk {
                flags: Some(if last { Flags::LastChunk as i32 } else { 0 }),
                offset: Some(offset),
                body: Some(body),
                ..PayloadChunk::default()
            }),
            ..PayloadTransferFrame::default()
        };
        self.send(&offline(v1_frame::FrameType::PayloadTransfer, |v1| v1.payload_transfer = Some(transfer))).await
    }

    pub async fn disconnect(&mut self) -> Result<()> {
        let frame = offline(v1_frame::FrameType::Disconnection, |v1| {
            v1.disconnection = Some(DisconnectionFrame::default());
        });
        self.send(&frame).await
    }

    /// The next payload event; keep-alives are answered here, and one is sent whenever the line has been quiet.
    pub async fn recv(&mut self) -> Result<Incoming> {
        let mut quiet = Duration::ZERO;
        loop {
            let raw = tokio::select! {
                next = self.frames.recv() => match next {
                    Some(raw) => raw?,
                    None => return Ok(Incoming::Disconnected),
                },
                () = tokio::time::sleep(KEEP_ALIVE) => {
                    quiet += KEEP_ALIVE;
                    if quiet >= SILENCE {
                        return Err(Error::Timeout);
                    }
                    self.keep_alive(false).await?;
                    continue;
                }
            };
            quiet = Duration::ZERO;
            let frame = OfflineFrame::decode(self.channel.open(&raw)?.as_slice())?;
            let v1 = frame.v1.ok_or(Error::Protocol("offline frame without v1"))?;
            match v1.r#type.and_then(|t| v1_frame::FrameType::try_from(t).ok()) {
                Some(v1_frame::FrameType::KeepAlive) => {
                    if !v1.keep_alive.and_then(|k| k.ack).unwrap_or(false) {
                        self.keep_alive(true).await?;
                    }
                }
                Some(v1_frame::FrameType::Disconnection) => return Ok(Incoming::Disconnected),
                Some(v1_frame::FrameType::PayloadTransfer) => {
                    let transfer = v1.payload_transfer.ok_or(Error::Protocol("payload transfer without body"))?;
                    if let Some(incoming) = self.payload(transfer)? {
                        return Ok(incoming);
                    }
                }
                _ => log::debug!("ignoring offline frame {:?}", v1.r#type),
            }
        }
    }

    async fn keep_alive(&mut self, ack: bool) -> Result<()> {
        let frame = offline(v1_frame::FrameType::KeepAlive, |v1| {
            v1.keep_alive = Some(KeepAliveFrame { ack: Some(ack), ..KeepAliveFrame::default() });
        });
        self.send(&frame).await
    }

    fn payload(&mut self, transfer: PayloadTransferFrame) -> Result<Option<Incoming>> {
        if transfer.packet_type.is_some_and(|t| t != PacketType::Data as i32) {
            return Ok(None);
        }
        let header = transfer.payload_header.ok_or(Error::Protocol("payload without header"))?;
        let chunk = transfer.payload_chunk.ok_or(Error::Protocol("payload without chunk"))?;
        let id = header.id.ok_or(Error::Protocol("payload without id"))?;
        let total = header.total_size.unwrap_or(0);
        let offset = chunk.offset.ok_or(Error::Protocol("chunk without offset"))?;
        let last = chunk.flags.unwrap_or(0) & Flags::LastChunk as i32 != 0;
        let body = chunk.body.unwrap_or_default();
        match header.r#type.and_then(|t| PayloadType::try_from(t).ok()) {
            Some(PayloadType::File) => Ok(Some(Incoming::FileChunk { id, total, offset, body, last })),
            Some(PayloadType::Bytes) => {
                if total > MAX_BYTES_PAYLOAD {
                    return Err(Error::Protocol("bytes payload too large"));
                }
                let buffer = self.assembling.entry(id).or_default();
                if usize::try_from(offset).ok() != Some(buffer.len()) {
                    return Err(Error::Protocol("bytes payload chunk out of order"));
                }
                buffer.extend_from_slice(&body);
                if i64::try_from(buffer.len()).map_or(true, |len| len > total) {
                    return Err(Error::Protocol("bytes payload longer than announced"));
                }
                if !last {
                    return Ok(None);
                }
                let data = self.assembling.remove(&id).unwrap_or_default();
                Ok(Some(Incoming::Bytes { id, data }))
            }
            _ => Err(Error::Protocol("unsupported payload type")),
        }
    }
}

pub fn random_id() -> Result<i64> {
    use ring::rand::SecureRandom as _;
    let mut bytes = [0; 8];
    ring::rand::SystemRandom::new().fill(&mut bytes).map_err(|_| Error::Random)?;
    Ok(i64::from_be_bytes(bytes))
}
