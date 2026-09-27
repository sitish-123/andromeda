//! TLS 1.3 with RFC 7250 raw public keys on both sides. Only Ed25519 is offered or accepted.

use std::fmt::Write as _;
use std::sync::Arc;

use rustls::client::danger::{HandshakeSignatureValid, ServerCertVerified, ServerCertVerifier};
use rustls::crypto::{WebPkiSupportedAlgorithms, ring as provider, verify_tls13_signature_with_raw_key};
use rustls::pki_types::{CertificateDer, PrivatePkcs8KeyDer, ServerName, SubjectPublicKeyInfoDer, UnixTime};
use rustls::server::danger::{ClientCertVerified, ClientCertVerifier};
use rustls::sign::CertifiedKey;
use rustls::{DigitallySignedStruct, DistinguishedName, SignatureScheme};

use crate::Error;
use crate::identity::{Identity, Spki};

/// What the phone accepts from the desktop during the handshake.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ServerPin {
    /// A known desktop: its SPKI fingerprint, from pairing or the QR code.
    Key([u8; 32]),
    /// Code pairing, before any key is known; SPAKE2 authenticates the session afterwards.
    Any,
}

const ANY_SERVER: &str = "pairing.link";

impl ServerPin {
    /// The TLS server name carrying this pin: the fingerprint as two 32-digit hex labels, since a DNS label holds at
    /// most 63 characters. One verifier then serves every desktop, which rustls requires to reuse cached sessions.
    pub fn server_name(self) -> String {
        match self {
            Self::Key(fingerprint) => format!("{}.{}.link", hex(&fingerprint[..16]), hex(&fingerprint[16..])),
            Self::Any => ANY_SERVER.to_owned(),
        }
    }

    fn from_server_name(name: &ServerName<'_>) -> Option<Self> {
        let ServerName::DnsName(name) = name else { return None };
        let name = name.as_ref();
        if name == ANY_SERVER {
            return Some(Self::Any);
        }
        let digits = name.strip_suffix(".link")?.replace('.', "");
        let bytes: Vec<u8> = (0..digits.len())
            .step_by(2)
            .map(|index| u8::from_str_radix(digits.get(index..index + 2)?, 16).ok())
            .collect::<Option<_>>()?;
        bytes.try_into().ok().map(Self::Key)
    }
}

fn hex(bytes: &[u8]) -> String {
    bytes.iter().fold(String::with_capacity(bytes.len() * 2), |mut out, byte| {
        let _ = write!(out, "{byte:02x}");
        out
    })
}

#[derive(Debug)]
struct PinnedServer {
    algorithms: WebPkiSupportedAlgorithms,
}

#[derive(Debug)]
struct AnyClient {
    algorithms: WebPkiSupportedAlgorithms,
}

pub fn certified_key(identity: &Identity) -> Result<Arc<CertifiedKey>, Error> {
    let der = PrivatePkcs8KeyDer::from(identity.pkcs8().to_vec());
    let key = provider::sign::any_eddsa_type(&der).map_err(|_| Error::BadKey)?;
    let spki = CertificateDer::from(identity.spki().as_der().to_vec());
    Ok(Arc::new(CertifiedKey::new(vec![spki], key)))
}

pub fn server_verifier() -> Arc<dyn ClientCertVerifier> {
    Arc::new(AnyClient { algorithms: provider::default_provider().signature_verification_algorithms })
}

pub fn client_verifier() -> Arc<dyn ServerCertVerifier> {
    Arc::new(PinnedServer { algorithms: provider::default_provider().signature_verification_algorithms })
}

/// The peer's key after a handshake, as quinn reports it.
pub fn peer_spki(identity: Option<Box<dyn std::any::Any>>) -> Result<Spki, Error> {
    let certs = identity.and_then(|any| any.downcast::<Vec<CertificateDer<'static>>>().ok()).ok_or(Error::BadKey)?;
    let first = certs.first().ok_or(Error::BadKey)?;
    Spki::from_der(first.to_vec())
}

fn check_key(der: &[u8]) -> Result<Spki, rustls::Error> {
    Spki::from_der(der.to_vec()).map_err(|_| rustls::Error::InvalidCertificate(rustls::CertificateError::BadEncoding))
}

fn verify_signature(
    algorithms: &WebPkiSupportedAlgorithms,
    message: &[u8],
    cert: &CertificateDer<'_>,
    dss: &DigitallySignedStruct,
) -> Result<HandshakeSignatureValid, rustls::Error> {
    verify_tls13_signature_with_raw_key(message, &SubjectPublicKeyInfoDer::from(cert.as_ref()), dss, algorithms)
}

impl ServerCertVerifier for PinnedServer {
    fn verify_server_cert(
        &self,
        end_entity: &CertificateDer<'_>,
        _intermediates: &[CertificateDer<'_>],
        server_name: &ServerName<'_>,
        _ocsp: &[u8],
        _now: UnixTime,
    ) -> Result<ServerCertVerified, rustls::Error> {
        let spki = check_key(end_entity)?;
        match ServerPin::from_server_name(server_name) {
            Some(ServerPin::Any) => Ok(ServerCertVerified::assertion()),
            Some(ServerPin::Key(expected)) if spki.fingerprint() == expected => Ok(ServerCertVerified::assertion()),
            _ => Err(rustls::Error::InvalidCertificate(rustls::CertificateError::ApplicationVerificationFailure)),
        }
    }

    fn verify_tls12_signature(
        &self,
        _message: &[u8],
        _cert: &CertificateDer<'_>,
        _dss: &DigitallySignedStruct,
    ) -> Result<HandshakeSignatureValid, rustls::Error> {
        Err(rustls::Error::PeerIncompatible(rustls::PeerIncompatible::Tls13RequiredForQuic))
    }

    fn verify_tls13_signature(
        &self,
        message: &[u8],
        cert: &CertificateDer<'_>,
        dss: &DigitallySignedStruct,
    ) -> Result<HandshakeSignatureValid, rustls::Error> {
        verify_signature(&self.algorithms, message, cert, dss)
    }

    fn supported_verify_schemes(&self) -> Vec<SignatureScheme> {
        vec![SignatureScheme::ED25519]
    }

    fn requires_raw_public_keys(&self) -> bool {
        true
    }
}

impl ClientCertVerifier for AnyClient {
    fn root_hint_subjects(&self) -> &[DistinguishedName] {
        &[]
    }

    /// Any well-formed key passes; the daemon authorises the key after the handshake.
    fn verify_client_cert(
        &self,
        end_entity: &CertificateDer<'_>,
        _intermediates: &[CertificateDer<'_>],
        _now: UnixTime,
    ) -> Result<ClientCertVerified, rustls::Error> {
        check_key(end_entity).map(|_| ClientCertVerified::assertion())
    }

    fn verify_tls12_signature(
        &self,
        _message: &[u8],
        _cert: &CertificateDer<'_>,
        _dss: &DigitallySignedStruct,
    ) -> Result<HandshakeSignatureValid, rustls::Error> {
        Err(rustls::Error::PeerIncompatible(rustls::PeerIncompatible::Tls13RequiredForQuic))
    }

    fn verify_tls13_signature(
        &self,
        message: &[u8],
        cert: &CertificateDer<'_>,
        dss: &DigitallySignedStruct,
    ) -> Result<HandshakeSignatureValid, rustls::Error> {
        verify_signature(&self.algorithms, message, cert, dss)
    }

    fn supported_verify_schemes(&self) -> Vec<SignatureScheme> {
        vec![SignatureScheme::ED25519]
    }

    fn requires_raw_public_keys(&self) -> bool {
        true
    }
}
