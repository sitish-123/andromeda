//! TLS for LocalSend: the server presents the self-signed certificate; as a client the daemon accepts exactly the
//! certificate whose SHA-256 is the fingerprint the peer announced.

use std::sync::Arc;

use anyhow::Context;
use rustls::client::danger::{HandshakeSignatureValid, ServerCertVerified, ServerCertVerifier};
use rustls::crypto::{CryptoProvider, ring as provider, verify_tls12_signature, verify_tls13_signature};
use rustls::pki_types::{CertificateDer, PrivateKeyDer, PrivatePkcs8KeyDer, ServerName, UnixTime};
use rustls::{DigitallySignedStruct, SignatureScheme};

use super::cert::{self, Identity};

fn ring() -> Arc<CryptoProvider> {
    Arc::new(provider::default_provider())
}

pub fn server(identity: &Identity) -> anyhow::Result<Arc<rustls::ServerConfig>> {
    let key = PrivateKeyDer::Pkcs8(PrivatePkcs8KeyDer::from(identity.pkcs8.clone()));
    let mut config = rustls::ServerConfig::builder_with_provider(ring())
        .with_protocol_versions(&[&rustls::version::TLS13])?
        .with_no_client_auth()
        .with_single_cert(vec![CertificateDer::from(identity.certificate.clone())], key)
        .context("the LocalSend certificate")?;
    config.alpn_protocols = vec![b"http/1.1".to_vec()];
    Ok(Arc::new(config))
}

/// A client config that pins `fingerprint`.
pub fn client(fingerprint: &str) -> anyhow::Result<Arc<rustls::ClientConfig>> {
    let verifier = Arc::new(Pinned { fingerprint: fingerprint.to_ascii_lowercase(), provider: ring() });
    let mut config = rustls::ClientConfig::builder_with_provider(ring())
        .with_protocol_versions(&[&rustls::version::TLS13])?
        .dangerous()
        .with_custom_certificate_verifier(verifier)
        .with_no_client_auth();
    config.alpn_protocols = vec![b"http/1.1".to_vec()];
    Ok(Arc::new(config))
}

#[derive(Debug)]
struct Pinned {
    fingerprint: String,
    provider: Arc<CryptoProvider>,
}

impl ServerCertVerifier for Pinned {
    fn verify_server_cert(
        &self,
        end_entity: &CertificateDer<'_>,
        _intermediates: &[CertificateDer<'_>],
        _server_name: &ServerName<'_>,
        _ocsp_response: &[u8],
        _now: UnixTime,
    ) -> Result<ServerCertVerified, rustls::Error> {
        if cert::fingerprint(end_entity) == self.fingerprint {
            Ok(ServerCertVerified::assertion())
        } else {
            Err(rustls::Error::General("not the announced LocalSend fingerprint".to_owned()))
        }
    }

    fn verify_tls12_signature(
        &self,
        message: &[u8],
        cert: &CertificateDer<'_>,
        dss: &DigitallySignedStruct,
    ) -> Result<HandshakeSignatureValid, rustls::Error> {
        verify_tls12_signature(message, cert, dss, &self.provider.signature_verification_algorithms)
    }

    fn verify_tls13_signature(
        &self,
        message: &[u8],
        cert: &CertificateDer<'_>,
        dss: &DigitallySignedStruct,
    ) -> Result<HandshakeSignatureValid, rustls::Error> {
        verify_tls13_signature(message, cert, dss, &self.provider.signature_verification_algorithms)
    }

    fn supported_verify_schemes(&self) -> Vec<SignatureScheme> {
        self.provider.signature_verification_algorithms.supported_schemes()
    }
}
