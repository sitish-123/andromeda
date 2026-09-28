//! Link v1 wire protocol, sans-IO: messages, framing, the pairing handshake, and the session rules. See
//! `link/ARCHITECTURE.md`.

pub mod clip;
pub mod frame;
pub mod limit;
pub mod message;
pub mod pairing;
pub mod session;
pub mod transfer;

pub const ALPN: &[u8] = b"umbriel-link/1";
pub const VERSION: u32 = 1;

/// QUIC application close codes; see `link/ARCHITECTURE.md`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum CloseCode {
    Done = 0,
    NotPaired = 1,
    Unpaired = 2,
    UnsupportedVersion = 3,
    PairingFailed = 4,
    ProtocolError = 5,
    Busy = 6,
}

impl CloseCode {
    pub fn from_u64(code: u64) -> Option<Self> {
        Some(match code {
            0 => Self::Done,
            1 => Self::NotPaired,
            2 => Self::Unpaired,
            3 => Self::UnsupportedVersion,
            4 => Self::PairingFailed,
            5 => Self::ProtocolError,
            6 => Self::Busy,
            _ => return None,
        })
    }

    pub fn reason(self) -> &'static str {
        match self {
            Self::Done => "done",
            Self::NotPaired => "not paired",
            Self::Unpaired => "unpaired",
            Self::UnsupportedVersion => "unsupported version",
            Self::PairingFailed => "pairing failed",
            Self::ProtocolError => "protocol error",
            Self::Busy => "busy",
        }
    }
}
