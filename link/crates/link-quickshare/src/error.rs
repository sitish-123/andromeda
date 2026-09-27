use std::io;

#[derive(Debug, thiserror::Error)]
pub enum Error {
    #[error("i/o: {0}")]
    Io(#[from] io::Error),
    #[error("frame of {0} bytes exceeds the limit")]
    FrameTooLarge(usize),
    #[error("malformed message: {0}")]
    Decode(#[from] prost::DecodeError),
    #[error("protocol violation: {0}")]
    Protocol(&'static str),
    #[error("key exchange failed: {0}")]
    Handshake(&'static str),
    #[error("message authentication failed")]
    Authentication,
    #[error("system randomness unavailable")]
    Random,
    #[error("the peer went silent")]
    Timeout,
}

pub type Result<T> = std::result::Result<T, Error>;
