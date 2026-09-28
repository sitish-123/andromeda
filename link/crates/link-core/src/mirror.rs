//! Mirroring the phone's screen: the video stream's framing on both ends. See "Mirroring" in `link/ARCHITECTURE.md`.

use std::sync::{Arc, Mutex};

use link_proto::message::{MAX_MIRROR_UNIT, Message, MirrorData};

use crate::Error;
use crate::control::write_frame;
use crate::session::SessionHandle;
use crate::wire::{RecvStream, SendStream};

/// Length, presentation time, flags.
const UNIT_HEADER: usize = 4 + 8 + 1;

/// One H.264 access unit in Annex B form.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Unit {
    pub pts_us: u64,
    /// [`link_proto::message::MIRROR_KEYFRAME`], [`link_proto::message::MIRROR_CONFIG`].
    pub flags: u8,
    pub data: Vec<u8>,
}

/// The phone's end: a unidirectional stream on the session, opened with its `mirror-data` header.
pub struct MirrorSender {
    send: SendStream,
}

impl MirrorSender {
    pub async fn open(session: &SessionHandle, width: u32, height: u32) -> Result<Self, Error> {
        let mut send = session.connection().open_uni().await?;
        write_frame(&mut send, Message::MirrorData(MirrorData { width, height }), session.tap()).await?;
        Ok(Self { send })
    }

    pub async fn send(&mut self, unit: &Unit) -> Result<(), Error> {
        let len = u32::try_from(unit.data.len()).ok().filter(|len| *len as usize <= MAX_MIRROR_UNIT);
        let len = len.ok_or(Error::Unexpected("oversized mirror unit"))?;
        let mut header = [0; UNIT_HEADER];
        header[..4].copy_from_slice(&len.to_be_bytes());
        header[4..12].copy_from_slice(&unit.pts_us.to_be_bytes());
        header[12] = unit.flags;
        self.send.write_all(&header).await?;
        self.send.write_all(&unit.data).await
    }

    pub fn finish(mut self) {
        if let Err(error) = self.send.finish() {
            log::debug!("finishing the mirror stream: {error}");
        }
    }
}

/// The desktop's end, after the transfer actor read the header.
pub struct MirrorReceiver {
    pub width: u32,
    pub height: u32,
    recv: RecvStream,
}

impl MirrorReceiver {
    pub(crate) fn new(recv: RecvStream, header: &MirrorData) -> Self {
        Self { width: header.width, height: header.height, recv }
    }

    /// The next unit, or None once the phone finished the stream.
    pub async fn next(&mut self) -> Result<Option<Unit>, Error> {
        let mut header = [0; UNIT_HEADER];
        match self.recv.read_exact(&mut header).await {
            Ok(()) => {}
            Err(Error::StreamEnded) => return Ok(None),
            Err(error) => return Err(error),
        }
        let len = u32::from_be_bytes([header[0], header[1], header[2], header[3]]) as usize;
        if len > MAX_MIRROR_UNIT {
            return Err(Error::Unexpected("oversized mirror unit"));
        }
        let mut pts = [0; 8];
        pts.copy_from_slice(&header[4..12]);
        let mut data = vec![0; len];
        self.recv.read_exact(&mut data).await?;
        Ok(Some(Unit { pts_us: u64::from_be_bytes(pts), flags: header[12], data }))
    }

    pub fn stop(&mut self) {
        self.recv.stop(0);
    }
}

/// A peer's video stream as a transfer event carries it: whoever handles the event takes it once.
#[derive(Clone)]
pub struct MirrorStream(Arc<Mutex<Option<MirrorReceiver>>>);

impl MirrorStream {
    pub(crate) fn new(receiver: MirrorReceiver) -> Self {
        Self(Arc::new(Mutex::new(Some(receiver))))
    }

    pub fn take(&self) -> Option<MirrorReceiver> {
        self.0.lock().ok()?.take()
    }
}

impl std::fmt::Debug for MirrorStream {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str("MirrorStream")
    }
}

impl PartialEq for MirrorStream {
    fn eq(&self, other: &Self) -> bool {
        Arc::ptr_eq(&self.0, &other.0)
    }
}

impl Eq for MirrorStream {}
