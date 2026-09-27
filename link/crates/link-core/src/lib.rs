//! The Link engine shared by `umbriel-linkd`, the headless phone, and the Android app. See `link/ARCHITECTURE.md`.

pub mod client;
pub mod control;
pub mod discovery;
pub mod identity;
pub mod net;
pub mod pairing;
pub mod phone;
pub mod reach;
pub mod session;
pub mod store;
pub mod tls;
pub mod transport;
pub mod uri;

pub use link_proto as proto;
use link_proto::CloseCode;
use link_proto::frame::FrameTooLarge;
use link_proto::message::{DecodeError, ShareRejected};
use link_proto::pairing::PairingError;
use link_proto::session::SessionError;

#[derive(Debug, thiserror::Error)]
pub enum Error {
    #[error("i/o: {0}")]
    Io(#[from] std::io::Error),
    #[error("tls: {0}")]
    Tls(#[from] rustls::Error),
    #[error("quic connect: {0}")]
    Connect(#[from] quinn::ConnectError),
    #[error("quic: {0}")]
    Connection(quinn::ConnectionError),
    #[error("peer closed the connection: {}", .0.reason())]
    Closed(CloseCode),
    #[error("control stream ended")]
    StreamEnded,
    #[error(transparent)]
    Frame(#[from] FrameTooLarge),
    #[error(transparent)]
    Decode(#[from] DecodeError),
    #[error(transparent)]
    Pairing(#[from] PairingError),
    #[error("timed out")]
    Timeout,
    #[error("unexpected {0} message")]
    Unexpected(&'static str),
    #[error("peer speaks protocol version {0}")]
    Version(u32),
    #[error("not a valid ed25519 key")]
    BadKey,
    #[error("not a device id")]
    BadDeviceId,
    #[error("bad pairing uri: {0}")]
    BadUri(&'static str),
    #[error("store: {0}")]
    Store(#[from] serde_json::Error),
    #[error("mdns: {0}")]
    Mdns(#[from] mdns_sd::Error),
    #[error("no address answered")]
    Unreachable,
    #[error("the code is six digits")]
    BadCode,
    #[error("no desktop is pairing")]
    NoPairingDesktop,
    #[error("more than one desktop is pairing")]
    ManyPairingDesktops,
    #[error("not paired with that device")]
    UnknownDevice,
    #[error("not connected")]
    NotConnected,
    #[error("the link client stopped")]
    Stopped,
    #[error(transparent)]
    Share(#[from] ShareRejected),
    #[error(transparent)]
    Session(#[from] SessionError),
}

impl From<quinn::ConnectionError> for Error {
    fn from(error: quinn::ConnectionError) -> Self {
        match &error {
            quinn::ConnectionError::ApplicationClosed(close) => {
                CloseCode::from_u64(close.error_code.into_inner()).map_or(Self::Connection(error), Self::Closed)
            }
            quinn::ConnectionError::TimedOut => Self::Timeout,
            _ => Self::Connection(error),
        }
    }
}

impl From<quinn::WriteError> for Error {
    fn from(error: quinn::WriteError) -> Self {
        match error {
            quinn::WriteError::ConnectionLost(lost) => lost.into(),
            _ => Self::StreamEnded,
        }
    }
}

impl From<quinn::ReadExactError> for Error {
    fn from(error: quinn::ReadExactError) -> Self {
        match error {
            quinn::ReadExactError::ReadError(quinn::ReadError::ConnectionLost(lost)) => lost.into(),
            _ => Self::StreamEnded,
        }
    }
}

impl From<quinn::ClosedStream> for Error {
    fn from(_: quinn::ClosedStream) -> Self {
        Self::StreamEnded
    }
}

impl From<tokio::time::error::Elapsed> for Error {
    fn from(_: tokio::time::error::Elapsed) -> Self {
        Self::Timeout
    }
}

/// Closes `connection` with a protocol close code.
pub fn close(connection: &quinn::Connection, code: CloseCode) {
    connection.close(quinn::VarInt::from_u32(code as u32), code.reason().as_bytes());
}

/// The close code an error should end its connection with, when the error is ours to report.
pub fn close_code_for(error: &Error) -> CloseCode {
    match error {
        Error::Pairing(PairingError::Mismatch) => CloseCode::PairingFailed,
        Error::Version(_) => CloseCode::UnsupportedVersion,
        _ => CloseCode::ProtocolError,
    }
}
