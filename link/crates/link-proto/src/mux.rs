//! Streams over one ordered byte stream, for links without QUIC (Bluetooth RFCOMM): the frames, their codec, and the
//! per-stream rules. See "Bluetooth" in `link/ARCHITECTURE.md`.
//!
//! A frame is a 9-byte header, `kind: u8`, `stream: u32`, `len: u32` (big-endian), then `len` bytes of payload.

pub const HEADER_LEN: usize = 9;
/// The largest DATA payload; smaller than a control frame, so the control stream never waits behind a whole chunk.
pub const MAX_DATA: usize = 16 * 1024;
/// Every other kind's payload is a code and a short reason at most.
pub const MAX_OTHER: usize = 256;
/// Bytes a sender may have in flight on one stream before the receiver grants more.
pub const WINDOW: u32 = 256 * 1024;
/// Streams a side may have open that the other opened: the control stream and eight bulk streams.
pub const MAX_PEER_STREAMS: usize = 9;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum Kind {
    /// Opens a bidirectional stream; only the control stream is one.
    OpenBi = 0,
    /// Opens a stream only its opener writes.
    OpenUni = 1,
    Data = 2,
    /// The writer is done; the reader has every byte.
    Fin = 3,
    /// The writer abandons the stream with a `u32` code; the reader drops what it has.
    Reset = 4,
    /// The reader wants no more bytes, with a `u32` code; the writer stops.
    Stop = 5,
    /// The reader grants `u32` more bytes of window.
    Credit = 6,
    /// Ends the connection: a `u32` close code, then a UTF-8 reason. Stream is 0.
    Close = 7,
    Ping = 8,
    Pong = 9,
}

impl Kind {
    fn from_u8(kind: u8) -> Option<Self> {
        Some(match kind {
            0 => Self::OpenBi,
            1 => Self::OpenUni,
            2 => Self::Data,
            3 => Self::Fin,
            4 => Self::Reset,
            5 => Self::Stop,
            6 => Self::Credit,
            7 => Self::Close,
            8 => Self::Ping,
            9 => Self::Pong,
            _ => return None,
        })
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Frame {
    OpenBi { stream: u32 },
    OpenUni { stream: u32 },
    Data { stream: u32, bytes: Vec<u8> },
    Fin { stream: u32 },
    Reset { stream: u32, code: u32 },
    Stop { stream: u32, code: u32 },
    Credit { stream: u32, bytes: u32 },
    Close { code: u32, reason: String },
    Ping,
    Pong,
}

#[derive(Debug, Clone, PartialEq, Eq, thiserror::Error)]
pub enum MuxError {
    #[error("unknown frame kind {0}")]
    UnknownKind(u8),
    #[error("{kind:?} frame of {len} bytes is over its limit")]
    TooLong { kind: Kind, len: usize },
    #[error("{0:?} frame has a malformed payload")]
    Malformed(Kind),
}

/// A parsed header: what follows and how long it is, checked before the payload is read.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Header {
    pub kind: Kind,
    pub stream: u32,
    pub len: usize,
}

impl Header {
    pub fn parse(bytes: [u8; HEADER_LEN]) -> Result<Self, MuxError> {
        let kind = Kind::from_u8(bytes[0]).ok_or(MuxError::UnknownKind(bytes[0]))?;
        let stream = u32::from_be_bytes([bytes[1], bytes[2], bytes[3], bytes[4]]);
        let len = usize::try_from(u32::from_be_bytes([bytes[5], bytes[6], bytes[7], bytes[8]])).unwrap_or(usize::MAX);
        let limit = if kind == Kind::Data { MAX_DATA } else { MAX_OTHER };
        if len > limit {
            return Err(MuxError::TooLong { kind, len });
        }
        Ok(Self { kind, stream, len })
    }
}

impl Frame {
    /// The frame a checked header and its payload make.
    pub fn decode(header: Header, payload: Vec<u8>) -> Result<Self, MuxError> {
        let Header { kind, stream, .. } = header;
        let code = || -> Result<u32, MuxError> {
            let bytes: [u8; 4] = payload.as_slice().try_into().map_err(|_| MuxError::Malformed(kind))?;
            Ok(u32::from_be_bytes(bytes))
        };
        let empty = || if payload.is_empty() { Ok(()) } else { Err(MuxError::Malformed(kind)) };
        Ok(match kind {
            Kind::OpenBi => empty().map(|()| Self::OpenBi { stream })?,
            Kind::OpenUni => empty().map(|()| Self::OpenUni { stream })?,
            Kind::Data if payload.is_empty() => return Err(MuxError::Malformed(kind)),
            Kind::Data => Self::Data { stream, bytes: payload },
            Kind::Fin => empty().map(|()| Self::Fin { stream })?,
            Kind::Reset => Self::Reset { stream, code: code()? },
            Kind::Stop => Self::Stop { stream, code: code()? },
            Kind::Credit => Self::Credit { stream, bytes: code()? },
            Kind::Close => {
                let (code, reason) = payload.split_first_chunk::<4>().ok_or(MuxError::Malformed(kind))?;
                let reason = String::from_utf8(reason.to_vec()).map_err(|_| MuxError::Malformed(kind))?;
                Self::Close { code: u32::from_be_bytes(*code), reason }
            }
            Kind::Ping => empty().map(|()| Self::Ping)?,
            Kind::Pong => empty().map(|()| Self::Pong)?,
        })
    }

    /// The header and payload in one buffer. A DATA payload over [`MAX_DATA`] or a reason over the limit is the
    /// caller's bug; DATA is split before it gets here, and reasons are ours.
    pub fn encode(&self) -> Vec<u8> {
        let (kind, stream, payload): (Kind, u32, std::borrow::Cow<'_, [u8]>) = match self {
            Self::OpenBi { stream } => (Kind::OpenBi, *stream, (&[][..]).into()),
            Self::OpenUni { stream } => (Kind::OpenUni, *stream, (&[][..]).into()),
            Self::Data { stream, bytes } => (Kind::Data, *stream, bytes.as_slice().into()),
            Self::Fin { stream } => (Kind::Fin, *stream, (&[][..]).into()),
            Self::Reset { stream, code } => (Kind::Reset, *stream, code.to_be_bytes().to_vec().into()),
            Self::Stop { stream, code } => (Kind::Stop, *stream, code.to_be_bytes().to_vec().into()),
            Self::Credit { stream, bytes } => (Kind::Credit, *stream, bytes.to_be_bytes().to_vec().into()),
            Self::Close { code, reason } => {
                let mut payload = code.to_be_bytes().to_vec();
                let cut =
                    (0..=reason.len().min(MAX_OTHER - 4)).rev().find(|&at| reason.is_char_boundary(at)).unwrap_or(0);
                payload.extend_from_slice(&reason.as_bytes()[..cut]);
                (Kind::Close, 0, payload.into())
            }
            Self::Ping => (Kind::Ping, 0, (&[][..]).into()),
            Self::Pong => (Kind::Pong, 0, (&[][..]).into()),
        };
        let len = u32::try_from(payload.len()).unwrap_or(u32::MAX);
        let mut frame = Vec::with_capacity(HEADER_LEN + payload.len());
        frame.push(kind as u8);
        frame.extend_from_slice(&stream.to_be_bytes());
        frame.extend_from_slice(&len.to_be_bytes());
        frame.extend_from_slice(&payload);
        frame
    }
}

/// Which side opened a stream: the client (the phone) opens even ids, the server odd ones, as in QUIC.
pub fn opened_by_client(stream: u32) -> bool {
    stream.is_multiple_of(2)
}
