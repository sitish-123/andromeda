//! Wire format between `andromeda-preview` and this helper: one `SOCK_SEQPACKET` message per request or reply,
//! six little-endian u32 words, and at most one fd (a sealed memfd with pixels or text).

use rustix::fd::{AsFd, BorrowedFd, OwnedFd};
use rustix::net::{
    RecvAncillaryBuffer, RecvAncillaryMessage, RecvFlags, SendAncillaryBuffer, SendAncillaryMessage, SendFlags,
};
use std::io::{IoSlice, IoSliceMut};
use std::mem::MaybeUninit;

pub const OP_INFO: u32 = 1;
/// a = page index, b = target width in pixels, c = max height in pixels.
pub const OP_RENDER: u32 = 2;
pub const OP_TEXT: u32 = 3;
/// Next video frame. Reply `OP_FRAME` or `OP_END`.
pub const OP_FRAME: u32 = 4;
/// a = frame id: the window no longer shows that frame, so its buffer can be reused.
pub const OP_RELEASE: u32 = 5;

/// a = kind (KIND_*), b = page count, c/d = first page size in 1/100 pt.
pub const OP_INFO_REPLY: u32 = 0x81;
/// a = page, b = width, c = height, d = stride; fd = ARGB32 premultiplied pixels (cairo layout).
pub const OP_PAGE: u32 = 0x82;
/// a = byte length, b = 1 when cut at the size limit; fd = UTF-8 text.
pub const OP_TEXT_REPLY: u32 = 0x83;
/// a = width, b = height, c = DRM fourcc, d = plane count, e = frame id. Payload: u64 modifier, u64 pts in ns,
/// then per plane u32 fd index, u32 offset, u32 stride. One fd per distinct buffer (dmabuf or sealed memfd).
pub const OP_FRAME_REPLY: u32 = 0x84;
/// End of stream.
pub const OP_END: u32 = 0x85;
/// a = ERR_*.
pub const OP_ERROR: u32 = 0xff;

pub const KIND_TEXT: u32 = 1;
pub const KIND_PDF: u32 = 2;
/// Info reply for media: b = width, c = height, d = duration in ms, e = FLAG_* bits.
pub const KIND_MEDIA: u32 = 3;
pub const FLAG_VIDEO: u32 = 1;
pub const FLAG_AUDIO: u32 = 2;
/// Set when frames come as dmabufs from a hardware decoder.
pub const FLAG_HW_DECODE: u32 = 4;

pub const ERR_BAD_REQUEST: u32 = 1;
pub const ERR_DECODE: u32 = 2;
pub const ERR_RANGE: u32 = 3;
pub const ERR_TOO_LARGE: u32 = 4;
pub const ERR_BINARY: u32 = 5;
/// The window holds too many frames: release some first.
pub const ERR_BUSY: u32 = 6;

pub const WORDS: usize = 6;
pub const SIZE: usize = WORDS * 4;
pub const MAX_PAYLOAD: usize = 256;
pub const MAX_FDS: usize = 4;

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Msg {
    pub op: u32,
    pub a: u32,
    pub b: u32,
    pub c: u32,
    pub d: u32,
    pub e: u32,
}

impl Msg {
    #[must_use]
    pub const fn new(op: u32, a: u32, b: u32, c: u32, d: u32) -> Self {
        Self { op, a, b, c, d, e: 0 }
    }

    #[must_use]
    pub const fn error(code: u32) -> Self {
        Self::new(OP_ERROR, code, 0, 0, 0)
    }

    #[must_use]
    pub fn encode(&self) -> [u8; SIZE] {
        let mut out = [0u8; SIZE];
        for (i, w) in [self.op, self.a, self.b, self.c, self.d, self.e].iter().enumerate() {
            out[i * 4..i * 4 + 4].copy_from_slice(&w.to_le_bytes());
        }
        out
    }

    #[must_use]
    pub fn decode(bytes: &[u8; SIZE]) -> Self {
        let w = |i: usize| u32::from_le_bytes([bytes[i * 4], bytes[i * 4 + 1], bytes[i * 4 + 2], bytes[i * 4 + 3]]);
        Self { op: w(0), a: w(1), b: w(2), c: w(3), d: w(4), e: w(5) }
    }
}

/// Sends one message with an optional fd.
///
/// # Errors
/// The socket error.
pub fn send(sock: BorrowedFd<'_>, msg: &Msg, fd: Option<BorrowedFd<'_>>) -> std::io::Result<()> {
    send_with(sock, msg, &[], fd.as_slice())
}

/// Sends one message with a payload after the header and up to `MAX_FDS` fds.
///
/// # Errors
/// The socket error.
pub fn send_with(sock: BorrowedFd<'_>, msg: &Msg, payload: &[u8], fds: &[BorrowedFd<'_>]) -> std::io::Result<()> {
    debug_assert!(payload.len() <= MAX_PAYLOAD && fds.len() <= MAX_FDS);
    let bytes = msg.encode();
    let mut space = [MaybeUninit::uninit(); rustix::cmsg_space!(ScmRights(MAX_FDS))];
    let mut control = SendAncillaryBuffer::new(&mut space);
    if !fds.is_empty() {
        control.push(SendAncillaryMessage::ScmRights(fds));
    }
    rustix::net::sendmsg(sock, &[IoSlice::new(&bytes), IoSlice::new(payload)], &mut control, SendFlags::NOSIGNAL)?;
    Ok(())
}

/// Receives one message; `None` when the peer closed the socket.
///
/// # Errors
/// The socket error, or `InvalidData` for a message of the wrong size.
pub fn recv(sock: BorrowedFd<'_>) -> std::io::Result<Option<(Msg, Option<OwnedFd>)>> {
    let mut bytes = [0u8; SIZE];
    let mut space = [MaybeUninit::uninit(); rustix::cmsg_space!(ScmRights(1))];
    let mut control = RecvAncillaryBuffer::new(&mut space);
    let got = rustix::net::recvmsg(
        sock.as_fd(),
        &mut [IoSliceMut::new(&mut bytes)],
        &mut control,
        RecvFlags::CMSG_CLOEXEC,
    )?;
    let mut fd = None;
    for message in control.drain() {
        if let RecvAncillaryMessage::ScmRights(fds) = message {
            for received in fds {
                fd.get_or_insert(received);
            }
        }
    }
    if got.bytes == 0 {
        return Ok(None);
    }
    if got.bytes != SIZE || got.flags.intersects(rustix::net::ReturnFlags::TRUNC | rustix::net::ReturnFlags::CTRUNC) {
        return Err(std::io::Error::from(std::io::ErrorKind::InvalidData));
    }
    Ok(Some((Msg::decode(&bytes), fd)))
}
