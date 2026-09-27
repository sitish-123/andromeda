//! How a Quick Share endpoint names itself: the mDNS instance name and the endpoint info carried in the TXT record and
//! in the connection request.

use base64::Engine as _;
use base64::engine::general_purpose::URL_SAFE_NO_PAD;
use ring::rand::{SecureRandom, SystemRandom};

use crate::error::{Error, Result};

/// `_` + the first 6 bytes of SHA-256("NearbySharing") in hex.
pub const SERVICE_TYPE: &str = "_FC9F5ED42C8A._tcp.local.";
const PCP: u8 = 0x23;
const SERVICE_ID: [u8; 3] = [0xFC, 0x9F, 0x5E];
pub const DEVICE_LAPTOP: u8 = 3;
/// A device type byte, 16 bytes of salt and encrypted key, a name length byte.
const NAME_OFFSET: usize = 18;
const MAX_NAME: usize = 255;

pub struct Endpoint {
    /// Four alphanumeric characters, fresh per process.
    pub id: [u8; 4],
    pub name: String,
    pub device_type: u8,
}

impl Endpoint {
    pub fn new(name: &str, device_type: u8) -> Result<Self> {
        const ALPHABET: &[u8] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
        let mut id = [0; 4];
        SystemRandom::new().fill(&mut id).map_err(|_| Error::Random)?;
        for byte in &mut id {
            *byte = ALPHABET[usize::from(*byte) % ALPHABET.len()];
        }
        Ok(Self { id, name: truncate(name, MAX_NAME).to_owned(), device_type })
    }

    pub fn id_str(&self) -> String {
        String::from_utf8_lossy(&self.id).into_owned()
    }

    pub fn instance_name(&self) -> String {
        let mut raw = vec![PCP];
        raw.extend_from_slice(&self.id);
        raw.extend_from_slice(&SERVICE_ID);
        raw.extend_from_slice(&[0, 0]);
        URL_SAFE_NO_PAD.encode(raw)
    }

    /// Visible, version 0: device type in bits 1-3 of the first byte, then a random salt and key, then the name.
    pub fn info(&self) -> Result<Vec<u8>> {
        let mut info = vec![self.device_type << 1];
        let mut salt = [0; 16];
        SystemRandom::new().fill(&mut salt).map_err(|_| Error::Random)?;
        info.extend_from_slice(&salt);
        info.push(u8::try_from(self.name.len()).map_err(|_| Error::Protocol("device name too long"))?);
        info.extend_from_slice(self.name.as_bytes());
        Ok(info)
    }

    pub fn txt_record(&self) -> Result<String> {
        Ok(URL_SAFE_NO_PAD.encode(self.info()?))
    }
}

pub struct Peer {
    pub name: String,
    pub device_type: u8,
}

pub fn parse_info(info: &[u8]) -> Result<Peer> {
    let first = *info.first().ok_or(Error::Protocol("empty endpoint info"))?;
    let len = usize::from(*info.get(NAME_OFFSET - 1).ok_or(Error::Protocol("endpoint info too short"))?);
    let name = info.get(NAME_OFFSET..NAME_OFFSET + len).ok_or(Error::Protocol("endpoint name truncated"))?;
    Ok(Peer { name: String::from_utf8_lossy(name).into_owned(), device_type: (first >> 1) & 7 })
}

fn truncate(s: &str, max: usize) -> &str {
    let mut end = s.len().min(max);
    while !s.is_char_boundary(end) {
        end -= 1;
    }
    &s[..end]
}
