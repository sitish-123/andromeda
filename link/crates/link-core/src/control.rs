//! The control stream: length-prefixed CBOR envelopes on the phone's first bidirectional stream.

use std::sync::Arc;
use std::time::Duration;

use link_proto::VERSION;
use link_proto::frame::{self, HEADER_LEN};
use link_proto::message::{Envelope, Hello, Message};
use tokio::sync::mpsc;

use crate::Error;
use crate::wire::{Connection, RecvStream, SendStream};

/// How long a peer may take for one handshake step before the attempt is abandoned.
pub const STEP_TIMEOUT: Duration = Duration::from_secs(10);

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Direction {
    Sent,
    Received,
}

/// Observes every CBOR envelope on a control stream; the E2E transcripts are written through it.
pub type Tap = Arc<dyn Fn(Direction, &[u8]) + Send + Sync>;

pub struct Control {
    writer: ControlWriter,
    reader: ControlReader,
}

pub struct ControlWriter {
    send: SendStream,
    next_id: u64,
    tap: Option<Tap>,
}

pub struct ControlReader {
    recv: RecvStream,
    tap: Option<Tap>,
}

impl Control {
    pub async fn open(connection: &Connection, tap: Option<Tap>) -> Result<Self, Error> {
        let (send, recv) = connection.open_bi().await?;
        Ok(Self::new(send, recv, tap))
    }

    pub async fn accept(connection: &Connection, tap: Option<Tap>) -> Result<Self, Error> {
        let (send, recv) = tokio::time::timeout(STEP_TIMEOUT, connection.accept_bi()).await??;
        Ok(Self::new(send, recv, tap))
    }

    fn new(send: SendStream, recv: RecvStream, tap: Option<Tap>) -> Self {
        Self { writer: ControlWriter { send, next_id: 0, tap: tap.clone() }, reader: ControlReader { recv, tap } }
    }

    /// Sends `message` and returns its envelope id.
    pub async fn send(&mut self, message: Message) -> Result<u64, Error> {
        self.writer.send(message).await
    }

    /// The next message, which must arrive within [`STEP_TIMEOUT`].
    pub async fn recv(&mut self) -> Result<Message, Error> {
        Ok(tokio::time::timeout(STEP_TIMEOUT, self.reader.recv()).await??.message)
    }

    /// Sends our hello and returns the peer's; the phone speaks first.
    pub async fn hello_as_client(&mut self, own: Hello) -> Result<Hello, Error> {
        self.send(Message::Hello(own)).await?;
        expect_hello(self.recv().await?)
    }

    pub async fn hello_as_server(&mut self, own: Hello) -> Result<Hello, Error> {
        let peer = expect_hello(self.recv().await?)?;
        self.send(Message::Hello(own)).await?;
        Ok(peer)
    }

    pub fn split(self) -> (ControlReader, ControlWriter) {
        (self.reader, self.writer)
    }

    pub fn tap(&self) -> Option<Tap> {
        self.reader.tap.clone()
    }
}

impl ControlWriter {
    pub async fn send(&mut self, message: Message) -> Result<u64, Error> {
        let id = self.next_id;
        self.next_id += 1;
        let bytes = frame::encode(&Envelope::new(id, message));
        observe(self.tap.as_ref(), Direction::Sent, &bytes[HEADER_LEN..]);
        self.send.write_all(&bytes).await?;
        Ok(id)
    }
}

impl ControlReader {
    /// The next envelope, waiting as long as the connection lives. Not cancel-safe: a frame read halfway is lost.
    pub async fn recv(&mut self) -> Result<Envelope, Error> {
        read_frame(&mut self.recv, self.tap.as_ref()).await
    }

    /// Reads envelopes into `inbox` until the stream fails; a closed inbox ends it too. Polled as one future, so a
    /// select beside it never cancels a read halfway.
    pub async fn pump(mut self, inbox: mpsc::Sender<Envelope>) -> Result<(), Error> {
        loop {
            let envelope = self.recv().await?;
            if inbox.send(envelope).await.is_err() {
                return Ok(());
            }
        }
    }
}

/// Reads one frame: the next control message, or the header that opens a bulk stream.
pub async fn read_frame(recv: &mut RecvStream, tap: Option<&Tap>) -> Result<Envelope, Error> {
    let mut header = [0; HEADER_LEN];
    recv.read_exact(&mut header).await?;
    let mut body = vec![0; frame::body_len(header)?];
    recv.read_exact(&mut body).await?;
    observe(tap, Direction::Received, &body);
    Ok(Envelope::from_cbor(&body)?)
}

/// Writes the frame that opens a bulk stream; its envelope id is always 0.
pub async fn write_frame(send: &mut SendStream, message: Message, tap: Option<&Tap>) -> Result<(), Error> {
    let bytes = frame::encode(&Envelope::new(0, message));
    observe(tap, Direction::Sent, &bytes[HEADER_LEN..]);
    send.write_all(&bytes).await?;
    Ok(())
}

fn observe(tap: Option<&Tap>, direction: Direction, body: &[u8]) {
    if let Some(tap) = tap {
        tap(direction, body);
    }
}

fn expect_hello(message: Message) -> Result<Hello, Error> {
    let Message::Hello(hello) = message else {
        return Err(Error::Unexpected(message.kind()));
    };
    if hello.version != VERSION {
        return Err(Error::Version(hello.version));
    }
    Ok(hello)
}
