//! A session's connection over either transport: QUIC on an IP path, or [`crate::mux`] over TLS on a byte stream
//! (Bluetooth). Only what the session, the transfers, and the pairing handshake use.

use std::ops::Deref;

use link_proto::CloseCode;

use crate::Error;
use crate::mux::{Closed, MuxConnection, MuxRecv, MuxSend, StreamError};

#[derive(Clone, Debug)]
pub enum Connection {
    Quic(quinn::Connection),
    Stream(MuxConnection),
}

pub enum SendStream {
    Quic(quinn::SendStream),
    Stream(MuxSend),
}

pub enum RecvStream {
    Quic(quinn::RecvStream),
    Stream(MuxRecv),
}

/// Bytes read from a stream, borrowed from quinn's buffer on QUIC.
pub enum Chunk {
    Quic(quinn::Chunk),
    Stream(Vec<u8>),
}

impl Deref for Chunk {
    type Target = [u8];

    fn deref(&self) -> &[u8] {
        match self {
            Self::Quic(chunk) => &chunk.bytes,
            Self::Stream(bytes) => bytes,
        }
    }
}

impl From<quinn::Connection> for Connection {
    fn from(connection: quinn::Connection) -> Self {
        Self::Quic(connection)
    }
}

impl Connection {
    pub async fn open_bi(&self) -> Result<(SendStream, RecvStream), Error> {
        Ok(match self {
            Self::Quic(connection) => {
                let (send, recv) = connection.open_bi().await?;
                (SendStream::Quic(send), RecvStream::Quic(recv))
            }
            Self::Stream(connection) => {
                let (send, recv) = connection.open_bi()?;
                (SendStream::Stream(send), RecvStream::Stream(recv))
            }
        })
    }

    pub async fn accept_bi(&self) -> Result<(SendStream, RecvStream), Error> {
        Ok(match self {
            Self::Quic(connection) => {
                let (send, recv) = connection.accept_bi().await?;
                (SendStream::Quic(send), RecvStream::Quic(recv))
            }
            Self::Stream(connection) => {
                let (send, recv) = connection.accept_bi().await?;
                (SendStream::Stream(send), RecvStream::Stream(recv))
            }
        })
    }

    pub async fn open_uni(&self) -> Result<SendStream, Error> {
        Ok(match self {
            Self::Quic(connection) => SendStream::Quic(connection.open_uni().await?),
            Self::Stream(connection) => SendStream::Stream(connection.open_uni()?),
        })
    }

    pub async fn accept_uni(&self) -> Result<RecvStream, Error> {
        Ok(match self {
            Self::Quic(connection) => RecvStream::Quic(connection.accept_uni().await?),
            Self::Stream(connection) => RecvStream::Stream(connection.accept_uni().await?),
        })
    }

    /// Closes with a protocol close code.
    pub fn close(&self, code: CloseCode) {
        match self {
            Self::Quic(connection) => connection.close(quinn::VarInt::from_u32(code as u32), code.reason().as_bytes()),
            Self::Stream(connection) => connection.close(code as u32, code.reason()),
        }
    }

    pub fn is_live(&self) -> bool {
        match self {
            Self::Quic(connection) => connection.close_reason().is_none(),
            Self::Stream(connection) => connection.close_reason().is_none(),
        }
    }

    /// Waits for the end and says how it came.
    pub async fn closed(&self) -> Error {
        match self {
            Self::Quic(connection) => connection.closed().await.into(),
            Self::Stream(connection) => connection.closed().await.into(),
        }
    }

    pub fn stable_id(&self) -> usize {
        match self {
            Self::Quic(connection) => connection.stable_id(),
            Self::Stream(connection) => connection.stable_id(),
        }
    }

    /// The QUIC connection, for what only it has: the TLS exporter pairing binds to, and the peer's address.
    pub fn quic(&self) -> Option<&quinn::Connection> {
        match self {
            Self::Quic(connection) => Some(connection),
            Self::Stream(_) => None,
        }
    }
}

impl SendStream {
    pub async fn write_all(&mut self, bytes: &[u8]) -> Result<(), Error> {
        match self {
            Self::Quic(stream) => Ok(stream.write_all(bytes).await?),
            Self::Stream(stream) => Ok(stream.write_all(bytes).await?),
        }
    }

    pub fn finish(&mut self) -> Result<(), Error> {
        match self {
            Self::Quic(stream) => Ok(stream.finish()?),
            Self::Stream(stream) => Ok(stream.finish()?),
        }
    }

    /// Waits until the peer stops the stream and returns its code; None once the stream is done or the connection
    /// ends.
    pub async fn stopped(&mut self) -> Option<u32> {
        match self {
            Self::Quic(stream) => {
                stream.stopped().await.ok().flatten().and_then(|code| u32::try_from(code.into_inner()).ok())
            }
            Self::Stream(stream) => stream.stopped().await,
        }
    }

    /// Abandons the stream with `code`; an already finished stream stays as it is.
    pub fn reset(&mut self, code: u32) {
        match self {
            Self::Quic(stream) => drop(stream.reset(quinn::VarInt::from_u32(code))),
            Self::Stream(stream) => drop(stream.reset(code)),
        }
    }
}

impl RecvStream {
    pub async fn read_exact(&mut self, buf: &mut [u8]) -> Result<(), Error> {
        match self {
            Self::Quic(stream) => Ok(stream.read_exact(buf).await?),
            Self::Stream(stream) => Ok(stream.read_exact(buf).await?),
        }
    }

    /// Up to `max` bytes in order, or None at the end of the stream.
    pub async fn read_chunk(&mut self, max: usize) -> Result<Option<Chunk>, Error> {
        match self {
            Self::Quic(stream) => {
                Ok(stream.read_chunk(max, true).await.map_err(|_| Error::StreamEnded)?.map(Chunk::Quic))
            }
            Self::Stream(stream) => Ok(stream.read_chunk(max).await?.map(Chunk::Stream)),
        }
    }

    pub fn stop(&mut self, code: u32) {
        match self {
            Self::Quic(stream) => drop(stream.stop(quinn::VarInt::from_u32(code))),
            Self::Stream(stream) => stream.stop(code),
        }
    }
}

impl From<Closed> for Error {
    fn from(closed: Closed) -> Self {
        match closed {
            Closed::Local => Self::NotConnected,
            Closed::Peer { code, reason } => CloseCode::from_u64(code.into())
                .map_or_else(|| Self::Io(std::io::Error::other(format!("closed with {code}: {reason}"))), Self::Closed),
            Closed::TimedOut => Self::Timeout,
            Closed::Violation(error) => Self::Mux(error),
            Closed::Lost(why) => Self::Io(std::io::Error::other(why)),
        }
    }
}

impl From<StreamError> for Error {
    fn from(error: StreamError) -> Self {
        match error {
            StreamError::ConnectionLost(closed) => closed.into(),
            StreamError::Stopped(_) | StreamError::Reset(_) | StreamError::Finished => Self::StreamEnded,
        }
    }
}
