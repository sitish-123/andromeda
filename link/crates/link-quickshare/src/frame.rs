//! Every message on the socket is a big-endian `u32` length and that many bytes.

use tokio::io::{AsyncRead, AsyncReadExt, AsyncWrite, AsyncWriteExt};

use crate::error::{Error, Result};

/// Above the largest frame a peer sends: a 512 KiB file chunk plus its encryption and framing.
pub const MAX_FRAME: usize = 5 * 1024 * 1024;

pub async fn read(stream: &mut (impl AsyncRead + Unpin)) -> Result<Vec<u8>> {
    let len = usize::try_from(stream.read_u32().await?).map_err(|_| Error::FrameTooLarge(usize::MAX))?;
    if len > MAX_FRAME {
        return Err(Error::FrameTooLarge(len));
    }
    let mut buf = vec![0; len];
    stream.read_exact(&mut buf).await?;
    Ok(buf)
}

pub async fn write(stream: &mut (impl AsyncWrite + Unpin), bytes: &[u8]) -> Result<()> {
    let len = u32::try_from(bytes.len()).map_err(|_| Error::FrameTooLarge(bytes.len()))?;
    stream.write_u32(len).await?;
    stream.write_all(bytes).await?;
    stream.flush().await?;
    Ok(())
}
