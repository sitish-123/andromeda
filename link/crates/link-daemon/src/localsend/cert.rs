//! The LocalSend server identity: a self-signed ECDSA P-256 certificate, made once and kept in the state directory.
//! Its SHA-256 is the fingerprint LocalSend peers pin. The DER is written here rather than with a certificate crate:
//! it is output only, a dozen fixed fields.

use std::fs::{self, OpenOptions};
use std::io::Write;
use std::os::unix::fs::OpenOptionsExt;
use std::path::Path;

use anyhow::Context;
use ring::rand::{SecureRandom, SystemRandom};
use ring::signature::{ECDSA_P256_SHA256_ASN1_SIGNING, EcdsaKeyPair, KeyPair};

const ECDSA_WITH_SHA256: &[u8] = &[0x06, 0x08, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04, 0x03, 0x02];
const EC_PUBLIC_KEY: &[u8] = &[0x06, 0x07, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x02, 0x01];
const PRIME256V1: &[u8] = &[0x06, 0x08, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x03, 0x01, 0x07];
const COMMON_NAME: &[u8] = &[0x06, 0x03, 0x55, 0x04, 0x03];

pub struct Identity {
    pub pkcs8: Vec<u8>,
    pub certificate: Vec<u8>,
}

impl Identity {
    /// `localsend.pk8` and `localsend.der` in `dir`, made on first use.
    pub fn load_or_create(dir: &Path) -> anyhow::Result<Self> {
        let (key_path, cert_path) = (dir.join("localsend.pk8"), dir.join("localsend.der"));
        if let (Ok(pkcs8), Ok(certificate)) = (fs::read(&key_path), fs::read(&cert_path)) {
            return Ok(Self { pkcs8, certificate });
        }
        let rng = SystemRandom::new();
        let pkcs8 = EcdsaKeyPair::generate_pkcs8(&ECDSA_P256_SHA256_ASN1_SIGNING, &rng)
            .map_err(|_| anyhow::anyhow!("generating the LocalSend key"))?
            .as_ref()
            .to_vec();
        let certificate = self_signed(&pkcs8, &rng)?;
        write_private(&key_path, &pkcs8)?;
        write_private(&cert_path, &certificate)?;
        Ok(Self { pkcs8, certificate })
    }

    /// SHA-256 of the certificate, lowercase hex: the LocalSend fingerprint.
    pub fn fingerprint(&self) -> String {
        fingerprint(&self.certificate)
    }
}

pub fn fingerprint(certificate: &[u8]) -> String {
    super::hex(ring::digest::digest(&ring::digest::SHA256, certificate).as_ref())
}

fn write_private(path: &Path, bytes: &[u8]) -> anyhow::Result<()> {
    let mut file = OpenOptions::new().write(true).create(true).truncate(true).mode(0o600).open(path)?;
    file.write_all(bytes)?;
    file.sync_all().with_context(|| format!("writing {}", path.display()))
}

fn self_signed(pkcs8: &[u8], rng: &SystemRandom) -> anyhow::Result<Vec<u8>> {
    let pair = EcdsaKeyPair::from_pkcs8(&ECDSA_P256_SHA256_ASN1_SIGNING, pkcs8, rng)
        .map_err(|_| anyhow::anyhow!("reading the LocalSend key"))?;
    let mut serial = [0; 8];
    rng.fill(&mut serial).map_err(|_| anyhow::anyhow!("system randomness unavailable"))?;
    // Positive and minimal: the first byte is below 0x80 and not zero.
    serial[0] = (serial[0] & 0x7f) | 0x10;
    let name = seq(&[&set(&[&seq(&[COMMON_NAME, &tlv(0x0c, b"umbriel-linkd")])])]);
    let validity = seq(&[&tlv(0x17, b"250101000000Z"), &tlv(0x18, b"99991231235959Z")]);
    let public_key = [&[0][..], pair.public_key().as_ref()].concat();
    let spki = seq(&[&seq(&[EC_PUBLIC_KEY, PRIME256V1]), &tlv(0x03, &public_key)]);
    let version = tlv(0xa0, &tlv(0x02, &[2]));
    let algorithm = seq(&[ECDSA_WITH_SHA256]);
    let tbs = seq(&[&version, &tlv(0x02, &serial), &algorithm, &name, &validity, &name, &spki]);
    let signature = pair.sign(rng, &tbs).map_err(|_| anyhow::anyhow!("signing the LocalSend certificate"))?;
    let signature = [&[0][..], signature.as_ref()].concat();
    Ok(seq(&[&tbs, &algorithm, &tlv(0x03, &signature)]))
}

fn tlv(tag: u8, content: &[u8]) -> Vec<u8> {
    let mut out = vec![tag];
    match content.len() {
        len @ 0..0x80 => out.push(u8::try_from(len).unwrap_or(0)),
        len => {
            let bytes: Vec<u8> = len.to_be_bytes().into_iter().skip_while(|byte| *byte == 0).collect();
            out.push(0x80 | u8::try_from(bytes.len()).unwrap_or(0));
            out.extend(bytes);
        }
    }
    out.extend_from_slice(content);
    out
}

fn seq(parts: &[&[u8]]) -> Vec<u8> {
    tlv(0x30, &parts.concat())
}

fn set(parts: &[&[u8]]) -> Vec<u8> {
    tlv(0x31, &parts.concat())
}
