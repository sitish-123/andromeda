//! The D2D channel after UKEY2: each offline frame is sequence-numbered, AES-256-CBC encrypted with a fresh IV, and
//! HMAC-SHA256 signed over header and body.

use aes::Aes256;
use cbc::cipher::block_padding::Pkcs7;
use cbc::cipher::{BlockDecryptMut, BlockEncryptMut, KeyIvInit};
use prost::Message as _;
use ring::hmac;
use ring::rand::{SecureRandom, SystemRandom};

use crate::error::{Error, Result};
use crate::ukey2::Keys;
use crate::wire::securegcm::{self, DeviceToDeviceMessage, GcmMetadata};
use crate::wire::securemessage::{EncScheme, Header, HeaderAndBody, SecureMessage, SigScheme};

pub struct Channel {
    encrypt: [u8; 32],
    decrypt: [u8; 32],
    send_mac: hmac::Key,
    recv_mac: hmac::Key,
    send_seq: i32,
    recv_seq: i32,
    rng: SystemRandom,
}

impl Channel {
    pub fn new(keys: &Keys) -> Self {
        Self {
            encrypt: keys.encrypt,
            decrypt: keys.decrypt,
            send_mac: hmac::Key::new(hmac::HMAC_SHA256, &keys.send_mac),
            recv_mac: hmac::Key::new(hmac::HMAC_SHA256, &keys.recv_mac),
            send_seq: 0,
            recv_seq: 0,
            rng: SystemRandom::new(),
        }
    }

    pub fn seal(&mut self, frame: &[u8]) -> Result<Vec<u8>> {
        self.send_seq = self.send_seq.checked_add(1).ok_or(Error::Protocol("sequence number exhausted"))?;
        let message = DeviceToDeviceMessage { message: Some(frame.to_vec()), sequence_number: Some(self.send_seq) };
        let mut iv = [0; 16];
        self.rng.fill(&mut iv).map_err(|_| Error::Random)?;
        let body = cbc::Encryptor::<Aes256>::new(&self.encrypt.into(), &iv.into())
            .encrypt_padded_vec_mut::<Pkcs7>(&message.encode_to_vec());
        let metadata = GcmMetadata { r#type: securegcm::Type::DeviceToDeviceMessage as i32, version: Some(1) };
        let header = Header {
            signature_scheme: SigScheme::HmacSha256 as i32,
            encryption_scheme: EncScheme::Aes256Cbc as i32,
            iv: Some(iv.to_vec()),
            public_metadata: Some(metadata.encode_to_vec()),
            ..Header::default()
        };
        let header_and_body = HeaderAndBody { header, body }.encode_to_vec();
        let signature = hmac::sign(&self.send_mac, &header_and_body).as_ref().to_vec();
        Ok(SecureMessage { header_and_body, signature }.encode_to_vec())
    }

    pub fn open(&mut self, raw: &[u8]) -> Result<Vec<u8>> {
        let message = SecureMessage::decode(raw)?;
        hmac::verify(&self.recv_mac, &message.header_and_body, &message.signature)
            .map_err(|_| Error::Authentication)?;
        let HeaderAndBody { header, body } = HeaderAndBody::decode(message.header_and_body.as_slice())?;
        if header.encryption_scheme != EncScheme::Aes256Cbc as i32
            || header.signature_scheme != SigScheme::HmacSha256 as i32
        {
            return Err(Error::Protocol("unsupported secure message scheme"));
        }
        let iv: [u8; 16] =
            header.iv.as_deref().and_then(|iv| iv.try_into().ok()).ok_or(Error::Protocol("missing or bad iv"))?;
        let plain = cbc::Decryptor::<Aes256>::new(&self.decrypt.into(), &iv.into())
            .decrypt_padded_vec_mut::<Pkcs7>(&body)
            .map_err(|_| Error::Authentication)?;
        let inner = DeviceToDeviceMessage::decode(plain.as_slice())?;
        self.recv_seq = self.recv_seq.checked_add(1).ok_or(Error::Protocol("sequence number exhausted"))?;
        if inner.sequence_number != Some(self.recv_seq) {
            return Err(Error::Protocol("out-of-order or replayed message"));
        }
        inner.message.ok_or(Error::Protocol("empty secure message"))
    }
}
