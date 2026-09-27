use std::fmt::{self, Write as _};
use std::fs::{self, OpenOptions};
use std::io::Write;
use std::os::unix::fs::OpenOptionsExt;
use std::path::Path;

use ring::rand::SystemRandom;
use ring::signature::{Ed25519KeyPair, KeyPair};
use serde::{Deserialize, Serialize};

use crate::Error;

const ED25519_SPKI_PREFIX: [u8; 12] = [0x30, 0x2a, 0x30, 0x05, 0x06, 0x03, 0x2b, 0x65, 0x70, 0x03, 0x21, 0x00];
const ED25519_SPKI_LEN: usize = ED25519_SPKI_PREFIX.len() + 32;

/// A DER `SubjectPublicKeyInfo` holding an Ed25519 key: what TLS raw public keys carry and what pairing pins.
#[derive(Clone, PartialEq, Eq, Hash)]
pub struct Spki(Vec<u8>);

/// The first 16 bytes of SHA-256 over a device's SPKI, as lowercase hex.
#[derive(Clone, PartialEq, Eq, Hash, PartialOrd, Ord, Serialize, Deserialize)]
#[serde(transparent)]
pub struct DeviceId(String);

/// A device's long-term Ed25519 key.
pub struct Identity {
    pkcs8: Vec<u8>,
    spki: Spki,
}

impl Spki {
    pub fn from_der(der: Vec<u8>) -> Result<Self, Error> {
        if der.len() != ED25519_SPKI_LEN || der[..ED25519_SPKI_PREFIX.len()] != ED25519_SPKI_PREFIX {
            return Err(Error::BadKey);
        }
        Ok(Self(der))
    }

    fn from_public_key(public: &[u8]) -> Self {
        Self([&ED25519_SPKI_PREFIX, public].concat())
    }

    pub fn as_der(&self) -> &[u8] {
        &self.0
    }

    pub fn fingerprint(&self) -> [u8; 32] {
        let digest = ring::digest::digest(&ring::digest::SHA256, &self.0);
        let mut out = [0; 32];
        out.copy_from_slice(digest.as_ref());
        out
    }

    pub fn device_id(&self) -> DeviceId {
        DeviceId::from_fingerprint(&self.fingerprint())
    }
}

impl fmt::Debug for Spki {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "Spki({})", self.device_id())
    }
}

impl DeviceId {
    pub fn from_fingerprint(fingerprint: &[u8; 32]) -> Self {
        let mut hex = String::with_capacity(32);
        for byte in &fingerprint[..16] {
            let _ = write!(hex, "{byte:02x}");
        }
        Self(hex)
    }

    pub fn parse(text: &str) -> Result<Self, Error> {
        let valid = text.len() == 32 && text.bytes().all(|byte| matches!(byte, b'0'..=b'9' | b'a'..=b'f'));
        if valid { Ok(Self(text.to_owned())) } else { Err(Error::BadDeviceId) }
    }

    pub fn as_str(&self) -> &str {
        &self.0
    }
}

impl fmt::Display for DeviceId {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.0)
    }
}

impl fmt::Debug for DeviceId {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.0)
    }
}

impl Identity {
    pub fn generate() -> Result<Self, Error> {
        let pkcs8 = Ed25519KeyPair::generate_pkcs8(&SystemRandom::new()).map_err(|_| Error::BadKey)?;
        Self::from_pkcs8(pkcs8.as_ref().to_vec())
    }

    pub fn from_pkcs8(pkcs8: Vec<u8>) -> Result<Self, Error> {
        let pair = Ed25519KeyPair::from_pkcs8(&pkcs8).map_err(|_| Error::BadKey)?;
        let spki = Spki::from_public_key(pair.public_key().as_ref());
        Ok(Self { pkcs8, spki })
    }

    /// Loads the key at `path`, creating it with mode 0600 on first use.
    pub fn load_or_create(path: &Path) -> Result<Self, Error> {
        match fs::read(path) {
            Ok(pkcs8) => return Self::from_pkcs8(pkcs8),
            Err(error) if error.kind() != std::io::ErrorKind::NotFound => return Err(error.into()),
            Err(_) => {}
        }
        let identity = Self::generate()?;
        let mut file = OpenOptions::new().write(true).create_new(true).mode(0o600).open(path)?;
        file.write_all(&identity.pkcs8)?;
        file.sync_all()?;
        Ok(identity)
    }

    pub fn pkcs8(&self) -> &[u8] {
        &self.pkcs8
    }

    pub fn spki(&self) -> &Spki {
        &self.spki
    }

    pub fn device_id(&self) -> DeviceId {
        self.spki.device_id()
    }
}
