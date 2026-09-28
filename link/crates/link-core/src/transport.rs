use std::net::{Ipv6Addr, SocketAddr};
use std::sync::Arc;
use std::time::Duration;

use link_proto::ALPN;
use quinn::crypto::rustls::{QuicClientConfig, QuicServerConfig};
use rustls::client::{AlwaysResolvesClientRawPublicKeys, ClientSessionMemoryCache, Resumption};
use rustls::crypto::ring as provider;
use rustls::server::AlwaysResolvesServerRawPublicKeys;

use crate::Error;
use crate::identity::Identity;
use crate::tls::{self, ServerPin};

pub const IDLE_TIMEOUT: Duration = Duration::from_secs(30);
pub const CONNECT_TIMEOUT: Duration = Duration::from_secs(5);
/// A present phone's PING interval; `link/ARCHITECTURE.md` (Session) says why a third of [`IDLE_TIMEOUT`].
pub const KEEP_ALIVE: Duration = Duration::from_secs(10);
/// Bulk streams a peer may open at once: a transfer streams four files, the rest is room for other transfers.
const MAX_BULK_STREAMS: u8 = 8;

/// A connection the phone dialled, handshake complete. `resumed` is set when the desktop accepted the TLS session
/// ticket (0-RTT), skipping the full handshake. No application data rides in 0-RTT, so nothing can be replayed.
pub struct Dialed {
    pub connection: quinn::Connection,
    pub resumed: bool,
}

/// The phone side: one UDP socket and one TLS session cache for every desktop it talks to.
#[derive(Clone)]
pub struct Dialer {
    endpoint: quinn::Endpoint,
    config: quinn::ClientConfig,
}

#[expect(clippy::expect_used, reason = "the constant idle timeout is far below QUIC's varint limit")]
fn transport_config(keep_alive: Option<Duration>) -> Arc<quinn::TransportConfig> {
    let mut config = quinn::TransportConfig::default();
    config.max_idle_timeout(Some(IDLE_TIMEOUT.try_into().expect("idle timeout fits a varint")));
    config.max_concurrent_uni_streams(MAX_BULK_STREAMS.into());
    config.keep_alive_interval(keep_alive);
    // Loss-based control collapses on the random loss of Wi-Fi (about 130 KB/s at 5% and 50 ms); BBR paces by the
    // measured bandwidth instead.
    config.congestion_controller_factory(Arc::new(quinn::congestion::BbrConfig::default()));
    Arc::new(config)
}

pub(crate) fn tls13_provider() -> Arc<rustls::crypto::CryptoProvider> {
    Arc::new(provider::default_provider())
}

pub fn server_endpoint(identity: &Identity, addr: SocketAddr) -> Result<quinn::Endpoint, Error> {
    Ok(punchable_server_endpoint(identity, addr)?.0)
}

/// A server endpoint and a [`Puncher`] sending from its socket.
pub fn punchable_server_endpoint(identity: &Identity, addr: SocketAddr) -> Result<(quinn::Endpoint, Puncher), Error> {
    let resolver = AlwaysResolvesServerRawPublicKeys::new(tls::certified_key(identity)?);
    let mut crypto = rustls::ServerConfig::builder_with_provider(tls13_provider())
        .with_protocol_versions(&[&rustls::version::TLS13])?
        .with_client_cert_verifier(tls::server_verifier())
        .with_cert_resolver(Arc::new(resolver));
    crypto.alpn_protocols = vec![ALPN.to_vec()];
    crypto.max_early_data_size = u32::MAX;
    let crypto = QuicServerConfig::try_from(crypto).map_err(|_| Error::BadKey)?;
    let mut config = quinn::ServerConfig::with_crypto(Arc::new(crypto));
    config.transport_config(transport_config(None));
    let socket = std::net::UdpSocket::bind(addr)?;
    let puncher = Puncher(Arc::new(socket.try_clone()?));
    let runtime = quinn::default_runtime().ok_or_else(|| std::io::Error::other("no async runtime"))?;
    let endpoint = quinn::Endpoint::new(quinn::EndpointConfig::default(), Some(config), socket, runtime)?;
    Ok((endpoint, puncher))
}

/// Sends from the server's own socket, so a stateful firewall in front of it takes a peer's dial from an address it
/// was sent to for a reply.
#[derive(Clone)]
pub struct Puncher(Arc<std::net::UdpSocket>);

impl Puncher {
    /// One byte to each address: not QUIC, so the peer's endpoint drops it.
    pub fn punch(&self, addresses: &[SocketAddr]) {
        let dual_stack = self.0.local_addr().is_ok_and(|local| local.is_ipv6());
        for &addr in addresses {
            let to = match addr {
                SocketAddr::V4(v4) if dual_stack => SocketAddr::new(v4.ip().to_ipv6_mapped().into(), v4.port()),
                addr => addr,
            };
            if let Err(error) = self.0.send_to(&[0], to) {
                log::info!("punching toward {addr}: {error}");
            }
        }
    }
}

impl Dialer {
    pub fn new(identity: &Identity) -> Result<Self, Error> {
        let dual_stack = SocketAddr::from((Ipv6Addr::UNSPECIFIED, 0));
        let endpoint =
            quinn::Endpoint::client(dual_stack).or_else(|_| quinn::Endpoint::client(([0, 0, 0, 0], 0).into()))?;
        Ok(Self { endpoint, config: client_config(identity)? })
    }

    /// The same socket and TLS session cache, dialling with keep-alive so the connection outlives idle periods.
    #[must_use]
    pub fn present(&self) -> Self {
        let mut config = self.config.clone();
        config.transport_config(transport_config(Some(KEEP_ALIVE)));
        Self { endpoint: self.endpoint.clone(), config }
    }

    /// Waits until every connection has sent its close; call before the process exits.
    pub async fn finish(&self) {
        self.endpoint.wait_idle().await;
    }

    /// The UDP port every dial leaves from.
    pub fn port(&self) -> Option<u16> {
        self.endpoint.local_addr().ok().map(|addr| addr.port())
    }

    pub async fn dial(&self, addr: SocketAddr, pin: ServerPin) -> Result<Dialed, Error> {
        let connecting = self.endpoint.connect_with(self.config.clone(), addr, &pin.server_name())?;
        match connecting.into_0rtt() {
            Ok((connection, accepted)) => {
                let resumed = tokio::time::timeout(CONNECT_TIMEOUT, accepted).await?;
                if let Some(reason) = connection.close_reason() {
                    return Err(reason.into());
                }
                Ok(Dialed { connection, resumed })
            }
            Err(connecting) => {
                let connection = tokio::time::timeout(CONNECT_TIMEOUT, connecting).await??;
                Ok(Dialed { connection, resumed: false })
            }
        }
    }
}

/// Built once per dialer: rustls reuses a cached session only with the same verifier and key resolver objects.
fn client_config(identity: &Identity) -> Result<quinn::ClientConfig, Error> {
    let resolver = AlwaysResolvesClientRawPublicKeys::new(tls::certified_key(identity)?);
    let mut crypto = rustls::ClientConfig::builder_with_provider(tls13_provider())
        .with_protocol_versions(&[&rustls::version::TLS13])?
        .dangerous()
        .with_custom_certificate_verifier(tls::client_verifier())
        .with_client_cert_resolver(Arc::new(resolver));
    crypto.alpn_protocols = vec![ALPN.to_vec()];
    crypto.enable_early_data = true;
    crypto.resumption = Resumption::store(Arc::new(ClientSessionMemoryCache::new(32)));
    let crypto = QuicClientConfig::try_from(crypto).map_err(|_| Error::BadKey)?;
    let mut config = quinn::ClientConfig::new(Arc::new(crypto));
    config.transport_config(transport_config(None));
    Ok(config)
}
