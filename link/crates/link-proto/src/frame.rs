use crate::message::Envelope;

pub const MAX_FRAME: usize = 64 * 1024;
pub const HEADER_LEN: usize = 4;

#[derive(Debug, thiserror::Error)]
#[error("frame of {0} bytes exceeds the {MAX_FRAME} byte limit")]
pub struct FrameTooLarge(pub usize);

pub fn encode(envelope: &Envelope) -> Vec<u8> {
    let body = envelope.to_cbor();
    let len = u32::try_from(body.len()).unwrap_or(u32::MAX);
    let mut frame = Vec::with_capacity(HEADER_LEN + body.len());
    frame.extend_from_slice(&len.to_be_bytes());
    frame.extend_from_slice(&body);
    frame
}

/// Length of the body that follows `header`, checked against [`MAX_FRAME`] before anything is allocated.
pub fn body_len(header: [u8; HEADER_LEN]) -> Result<usize, FrameTooLarge> {
    let len = usize::try_from(u32::from_be_bytes(header)).unwrap_or(usize::MAX);
    if len > MAX_FRAME {
        return Err(FrameTooLarge(len));
    }
    Ok(len)
}
