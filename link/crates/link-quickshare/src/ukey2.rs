//! UKEY2 with P-256 and SHA-512 commitments, as Nearby Connections runs it, and the key schedule of its
//! `AES_256_CBC-HMAC_SHA256` next protocol.

use prost::Message as _;
use ring::agreement::{self, ECDH_P256, EphemeralPrivateKey, UnparsedPublicKey};
use ring::digest::{self, SHA256, SHA512};
use ring::hkdf;
use ring::rand::{SecureRandom, SystemRandom};
use tokio::io::{AsyncRead, AsyncWrite};

use crate::error::{Error, Result};
use crate::frame;
use crate::wire::securegcm::{
    Ukey2ClientFinished, Ukey2ClientInit, Ukey2HandshakeCipher, Ukey2Message, Ukey2ServerInit, ukey2_client_init,
    ukey2_message,
};
use crate::wire::securemessage::{EcP256PublicKey, GenericPublicKey, PublicKeyType};

const NEXT_PROTOCOL: &str = "AES_256_CBC-HMAC_SHA256";
const D2D_SALT: [u8; 32] = [
    0x82, 0xAA, 0x55, 0xA0, 0xD3, 0x97, 0xF8, 0x83, 0x46, 0xCA, 0x1C, 0xEE, 0x8D, 0x39, 0x09, 0xB9, 0x5F, 0x13, 0xFA,
    0x7D, 0xEB, 0x1D, 0x4A, 0xB3, 0x83, 0x76, 0xB8, 0x25, 0x6D, 0xA8, 0x55, 0x10,
];

/// The secure channel's keys, already oriented for this side.
pub struct Keys {
    pub encrypt: [u8; 32],
    pub decrypt: [u8; 32],
    pub send_mac: [u8; 32],
    pub recv_mac: [u8; 32],
}

pub struct Handshake {
    pub keys: Keys,
    /// The 4-digit code both screens show; equal codes mean no one is in the middle.
    pub pin: String,
}

pub async fn server(stream: &mut (impl AsyncRead + AsyncWrite + Unpin)) -> Result<Handshake> {
    let client_init_raw = frame::read(stream).await?;
    let client_init = Ukey2ClientInit::decode(expect(&client_init_raw, ukey2_message::Type::ClientInit)?.as_slice())?;
    if client_init.version != Some(1) || client_init.random.as_ref().map(Vec::len) != Some(32) {
        return Err(Error::Handshake("unsupported client init"));
    }
    if client_init.next_protocol.as_deref() != Some(NEXT_PROTOCOL) {
        return Err(Error::Handshake("unsupported next protocol"));
    }
    let commitment = client_init
        .cipher_commitments
        .iter()
        .find(|c| c.handshake_cipher == Some(Ukey2HandshakeCipher::P256Sha512 as i32))
        .and_then(|c| c.commitment.clone())
        .ok_or(Error::Handshake("no P-256 commitment"))?;

    let rng = SystemRandom::new();
    let private = EphemeralPrivateKey::generate(&ECDH_P256, &rng).map_err(|_| Error::Random)?;
    let public = private.compute_public_key().map_err(|_| Error::Random)?;
    let server_init = Ukey2ServerInit {
        version: Some(1),
        random: Some(random32(&rng)?.to_vec()),
        handshake_cipher: Some(Ukey2HandshakeCipher::P256Sha512 as i32),
        public_key: Some(encode_public_key(public.as_ref())?),
    };
    let server_init_raw = wrap(ukey2_message::Type::ServerInit, &server_init.encode_to_vec());
    frame::write(stream, &server_init_raw).await?;

    let client_finish_raw = frame::read(stream).await?;
    let digest = digest::digest(&SHA512, &client_finish_raw);
    if !constant_eq(digest.as_ref(), &commitment) {
        return Err(Error::Handshake("client finish does not match its commitment"));
    }
    let client_finish =
        Ukey2ClientFinished::decode(expect(&client_finish_raw, ukey2_message::Type::ClientFinish)?.as_slice())?;
    let peer = decode_public_key(client_finish.public_key.as_deref().ok_or(Error::Handshake("no client key"))?)?;
    let dhs = agree(private, &peer)?;
    derive(&dhs, &client_init_raw, &server_init_raw, true)
}

pub async fn client(stream: &mut (impl AsyncRead + AsyncWrite + Unpin)) -> Result<Handshake> {
    let rng = SystemRandom::new();
    let private = EphemeralPrivateKey::generate(&ECDH_P256, &rng).map_err(|_| Error::Random)?;
    let public = private.compute_public_key().map_err(|_| Error::Random)?;
    let finish = Ukey2ClientFinished { public_key: Some(encode_public_key(public.as_ref())?) };
    let client_finish_raw = wrap(ukey2_message::Type::ClientFinish, &finish.encode_to_vec());
    let client_init = Ukey2ClientInit {
        version: Some(1),
        random: Some(random32(&rng)?.to_vec()),
        cipher_commitments: vec![ukey2_client_init::CipherCommitment {
            handshake_cipher: Some(Ukey2HandshakeCipher::P256Sha512 as i32),
            commitment: Some(digest::digest(&SHA512, &client_finish_raw).as_ref().to_vec()),
        }],
        next_protocol: Some(NEXT_PROTOCOL.to_owned()),
    };
    let client_init_raw = wrap(ukey2_message::Type::ClientInit, &client_init.encode_to_vec());
    frame::write(stream, &client_init_raw).await?;

    let server_init_raw = frame::read(stream).await?;
    let server_init = Ukey2ServerInit::decode(expect(&server_init_raw, ukey2_message::Type::ServerInit)?.as_slice())?;
    if server_init.version != Some(1)
        || server_init.handshake_cipher != Some(Ukey2HandshakeCipher::P256Sha512 as i32)
        || server_init.random.as_ref().map(Vec::len) != Some(32)
    {
        return Err(Error::Handshake("unsupported server init"));
    }
    let peer = decode_public_key(server_init.public_key.as_deref().ok_or(Error::Handshake("no server key"))?)?;
    frame::write(stream, &client_finish_raw).await?;
    let dhs = agree(private, &peer)?;
    derive(&dhs, &client_init_raw, &server_init_raw, false)
}

fn wrap(kind: ukey2_message::Type, data: &[u8]) -> Vec<u8> {
    Ukey2Message { message_type: Some(kind as i32), message_data: Some(data.to_vec()) }.encode_to_vec()
}

fn expect(raw: &[u8], kind: ukey2_message::Type) -> Result<Vec<u8>> {
    let message = Ukey2Message::decode(raw)?;
    if message.message_type != Some(kind as i32) {
        return Err(Error::Handshake("unexpected message type"));
    }
    message.message_data.ok_or(Error::Handshake("empty message"))
}

fn random32(rng: &SystemRandom) -> Result<[u8; 32]> {
    let mut bytes = [0; 32];
    rng.fill(&mut bytes).map_err(|_| Error::Random)?;
    Ok(bytes)
}

/// Coordinates travel as signed big-endian integers: a leading zero keeps the top bit from reading as a sign.
fn encode_public_key(uncompressed: &[u8]) -> Result<Vec<u8>> {
    let [0x04, coordinates @ ..] = uncompressed else { return Err(Error::Handshake("unexpected key encoding")) };
    let signed = |c: &[u8]| if c[0] & 0x80 == 0 { c.to_vec() } else { [&[0][..], c].concat() };
    let (x, y) = coordinates.split_at(32);
    let key = GenericPublicKey {
        r#type: PublicKeyType::EcP256 as i32,
        ec_p256_public_key: Some(EcP256PublicKey { x: signed(x), y: signed(y) }),
        ..GenericPublicKey::default()
    };
    Ok(key.encode_to_vec())
}

fn decode_public_key(raw: &[u8]) -> Result<Vec<u8>> {
    let key = GenericPublicKey::decode(raw)?;
    let point = key.ec_p256_public_key.ok_or(Error::Handshake("not a P-256 key"))?;
    let mut uncompressed = vec![0x04];
    for coordinate in [&point.x, &point.y] {
        let trimmed = coordinate.strip_prefix(&[0][..]).unwrap_or(coordinate);
        if trimmed.len() > 32 {
            return Err(Error::Handshake("coordinate too long"));
        }
        uncompressed.extend(std::iter::repeat_n(0, 32 - trimmed.len()));
        uncompressed.extend_from_slice(trimmed);
    }
    Ok(uncompressed)
}

fn agree(private: EphemeralPrivateKey, peer: &[u8]) -> Result<[u8; 32]> {
    agreement::agree_ephemeral(private, &UnparsedPublicKey::new(&ECDH_P256, peer), |shared| {
        let mut dhs = [0; 32];
        dhs.copy_from_slice(digest::digest(&SHA256, shared).as_ref());
        dhs
    })
    .map_err(|_| Error::Handshake("invalid peer key"))
}

fn derive(dhs: &[u8], client_init: &[u8], server_init: &[u8], is_server: bool) -> Result<Handshake> {
    let info = [client_init, server_init].concat();
    let auth = hkdf32(dhs, b"UKEY2 v1 auth", &info)?;
    let next = hkdf32(dhs, b"UKEY2 v1 next", &info)?;
    let d2d_client = hkdf32(&next, &D2D_SALT, b"client")?;
    let d2d_server = hkdf32(&next, &D2D_SALT, b"server")?;
    let salt = digest::digest(&SHA256, b"SecureMessage");
    let client_enc = hkdf32(&d2d_client, salt.as_ref(), b"ENC:2")?;
    let client_mac = hkdf32(&d2d_client, salt.as_ref(), b"SIG:1")?;
    let server_enc = hkdf32(&d2d_server, salt.as_ref(), b"ENC:2")?;
    let server_mac = hkdf32(&d2d_server, salt.as_ref(), b"SIG:1")?;
    let keys = if is_server {
        Keys { encrypt: server_enc, decrypt: client_enc, send_mac: server_mac, recv_mac: client_mac }
    } else {
        Keys { encrypt: client_enc, decrypt: server_enc, send_mac: client_mac, recv_mac: server_mac }
    };
    Ok(Handshake { keys, pin: pin(&auth) })
}

struct Len32;

impl hkdf::KeyType for Len32 {
    fn len(&self) -> usize {
        32
    }
}

fn hkdf32(ikm: &[u8], salt: &[u8], info: &[u8]) -> Result<[u8; 32]> {
    let prk = hkdf::Salt::new(hkdf::HKDF_SHA256, salt).extract(ikm);
    let info = [info];
    let okm = prk.expand(&info, Len32).map_err(|_| Error::Handshake("hkdf"))?;
    let mut out = [0; 32];
    okm.fill(&mut out).map_err(|_| Error::Handshake("hkdf"))?;
    Ok(out)
}

/// Android's PIN: a base-31 polynomial of the auth string's signed bytes, modulo 9973.
fn pin(auth: &[u8]) -> String {
    const MODULUS: i64 = 9973;
    let (mut hash, mut multiplier) = (0_i64, 1_i64);
    for &byte in auth {
        hash = (hash + i64::from(byte.cast_signed()) * multiplier) % MODULUS;
        multiplier = (multiplier * 31) % MODULUS;
    }
    format!("{:04}", hash.abs())
}

fn constant_eq(a: &[u8], b: &[u8]) -> bool {
    a.len() == b.len() && a.iter().zip(b).fold(0, |acc, (x, y)| acc | (x ^ y)) == 0
}
