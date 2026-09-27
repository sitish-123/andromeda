//! The pairing handshake of `link/ARCHITECTURE.md`: symmetric SPAKE2 bound to the TLS session, then key confirmation.
//! Typestates make an out-of-order step unrepresentable; the driver rejects unexpected message types.

use ring::{hkdf, hmac};
use spake2::{Ed25519Group, Identity, Password, Spake2};

use crate::message::{PairConfirm, PairMethod, PairSpake};

pub const SPAKE_MSG_LEN: usize = 33;
pub const MAC_LEN: usize = 32;
pub const EXPORTER_LABEL: &[u8] = b"EXPORTER-umbriel-link-pair";
pub const EXPORTER_LEN: usize = 32;
const TRANSCRIPT_PREFIX: &[u8] = b"umbriel-link-pair-v1";

/// A pairing secret: the ASCII digits of the typed code, or the raw bytes of the QR secret.
pub struct Secret(Vec<u8>);

impl Secret {
    pub fn new(bytes: Vec<u8>) -> Self {
        Self(bytes)
    }
}

/// Both secrets of one pairing window; the phone's `method` says which one it holds.
pub struct Secrets {
    pub code: Secret,
    pub qr: Secret,
}

#[derive(Debug, PartialEq, Eq, thiserror::Error)]
pub enum PairingError {
    #[error("pairing message is malformed")]
    Malformed,
    #[error("pairing confirmation does not match")]
    Mismatch,
}

/// The SPAKE2 identity and confirmation transcript. Binding the TLS exporter defeats a relay that terminates TLS.
pub fn transcript(exporter: &[u8], server_spki: &[u8], client_spki: &[u8]) -> Vec<u8> {
    [TRANSCRIPT_PREFIX, exporter, server_spki, client_spki].concat()
}

pub struct ClientPairing {
    spake: Spake2<Ed25519Group>,
    transcript: Vec<u8>,
}

pub struct ClientConfirming {
    server_key: hmac::Key,
    transcript: Vec<u8>,
}

pub struct ServerPairing {
    transcript: Vec<u8>,
}

pub struct ServerConfirming {
    client_key: hmac::Key,
    server_key: hmac::Key,
    transcript: Vec<u8>,
}

impl ClientPairing {
    pub fn start(secret: &Secret, method: PairMethod, transcript: Vec<u8>) -> (Self, PairSpake) {
        let (spake, msg) = start_spake(secret, &transcript);
        (Self { spake, transcript }, PairSpake { method: Some(method), msg })
    }

    pub fn on_server_spake(self, server: &PairSpake) -> Result<(ClientConfirming, PairConfirm), PairingError> {
        let shared = self.spake.finish(&server.msg).map_err(|_| PairingError::Malformed)?;
        let (client_key, server_key) = confirm_keys(&shared);
        let confirm = PairConfirm { mac: sign(&client_key, &self.transcript) };
        Ok((ClientConfirming { server_key, transcript: self.transcript }, confirm))
    }
}

impl ClientConfirming {
    pub fn on_server_confirm(self, server: &PairConfirm) -> Result<(), PairingError> {
        hmac::verify(&self.server_key, &self.transcript, &server.mac).map_err(|_| PairingError::Mismatch)
    }
}

impl ServerPairing {
    pub fn new(transcript: Vec<u8>) -> Self {
        Self { transcript }
    }

    pub fn on_client_spake(
        self,
        secrets: &Secrets,
        client: &PairSpake,
    ) -> Result<(ServerConfirming, PairSpake), PairingError> {
        let secret = match client.method {
            Some(PairMethod::Code) => &secrets.code,
            Some(PairMethod::Qr) => &secrets.qr,
            None => return Err(PairingError::Malformed),
        };
        let (spake, msg) = start_spake(secret, &self.transcript);
        let shared = spake.finish(&client.msg).map_err(|_| PairingError::Malformed)?;
        let (client_key, server_key) = confirm_keys(&shared);
        let confirming = ServerConfirming { client_key, server_key, transcript: self.transcript };
        Ok((confirming, PairSpake { method: None, msg }))
    }
}

impl ServerConfirming {
    pub fn on_client_confirm(self, client: &PairConfirm) -> Result<PairConfirm, PairingError> {
        hmac::verify(&self.client_key, &self.transcript, &client.mac).map_err(|_| PairingError::Mismatch)?;
        Ok(PairConfirm { mac: sign(&self.server_key, &self.transcript) })
    }
}

fn start_spake(secret: &Secret, transcript: &[u8]) -> (Spake2<Ed25519Group>, Vec<u8>) {
    Spake2::<Ed25519Group>::start_symmetric(&Password::new(&secret.0), &Identity::new(transcript))
}

#[expect(clippy::expect_used, reason = "HKDF-SHA256 can always expand to one SHA-256 output")]
fn confirm_keys(shared: &[u8]) -> (hmac::Key, hmac::Key) {
    let prk = hkdf::Salt::new(hkdf::HKDF_SHA256, &[]).extract(shared);
    let derive = |info: &[u8]| -> hmac::Key {
        prk.expand(&[info], hmac::HMAC_SHA256).expect("one hash length is a valid HKDF output").into()
    };
    (derive(b"confirm client"), derive(b"confirm server"))
}

fn sign(key: &hmac::Key, transcript: &[u8]) -> Vec<u8> {
    hmac::sign(key, transcript).as_ref().to_vec()
}

#[cfg(test)]
mod tests {
    use super::*;

    fn secrets() -> Secrets {
        Secrets { code: Secret::new(b"123456".to_vec()), qr: Secret::new(vec![7; 16]) }
    }

    fn run(client_secret: &Secret, method: PairMethod, client_t: &[u8], server_t: &[u8]) -> Result<(), PairingError> {
        let (client, spake) = ClientPairing::start(client_secret, method, client_t.to_vec());
        let (server, server_spake) = ServerPairing::new(server_t.to_vec()).on_client_spake(&secrets(), &spake)?;
        let (client, confirm) = client.on_server_spake(&server_spake)?;
        let server_confirm = server.on_client_confirm(&confirm)?;
        client.on_server_confirm(&server_confirm)
    }

    #[test]
    fn matching_code_and_session_pair() {
        let t = transcript(&[1; 32], b"server", b"client");
        assert_eq!(run(&Secret::new(b"123456".to_vec()), PairMethod::Code, &t, &t), Ok(()));
        assert_eq!(run(&Secret::new(vec![7; 16]), PairMethod::Qr, &t, &t), Ok(()));
    }

    #[test]
    fn wrong_code_fails_at_the_server() {
        let t = transcript(&[1; 32], b"server", b"client");
        assert_eq!(run(&Secret::new(b"123457".to_vec()), PairMethod::Code, &t, &t), Err(PairingError::Mismatch));
    }

    #[test]
    fn relay_with_its_own_tls_sessions_fails_despite_the_right_code() {
        let phone_leg = transcript(&[1; 32], b"relay", b"client");
        let desktop_leg = transcript(&[2; 32], b"server", b"relay");
        let code = Secret::new(b"123456".to_vec());
        assert_eq!(run(&code, PairMethod::Code, &phone_leg, &desktop_leg), Err(PairingError::Mismatch));
    }

    #[test]
    fn replayed_server_messages_fail() -> Result<(), PairingError> {
        let t = transcript(&[1; 32], b"server", b"client");
        let code = Secret::new(b"123456".to_vec());
        let (_, old_spake) = ClientPairing::start(&code, PairMethod::Code, t.clone());
        let (old_server, old_server_spake) = ServerPairing::new(t.clone()).on_client_spake(&secrets(), &old_spake)?;
        let (fresh_client, _) = ClientPairing::start(&code, PairMethod::Code, t);
        let (_, fresh_confirm) = fresh_client.on_server_spake(&old_server_spake)?;
        assert_eq!(old_server.on_client_confirm(&fresh_confirm).map(|_| ()), Err(PairingError::Mismatch));
        Ok(())
    }

    #[test]
    fn missing_method_or_bad_point_is_malformed() {
        let t = transcript(&[1; 32], b"server", b"client");
        let no_method = PairSpake { method: None, msg: vec![0x53; SPAKE_MSG_LEN] };
        assert!(matches!(
            ServerPairing::new(t.clone()).on_client_spake(&secrets(), &no_method),
            Err(PairingError::Malformed)
        ));
        let bad_point = PairSpake { method: Some(PairMethod::Code), msg: vec![0; 3] };
        assert!(matches!(ServerPairing::new(t).on_client_spake(&secrets(), &bad_point), Err(PairingError::Malformed)));
    }
}
