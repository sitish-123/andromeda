//! Link over a byte stream (Bluetooth RFCOMM): TLS 1.3 with the same raw public keys and verifiers as QUIC, ALPN
//! [`STREAM_ALPN`], then [`MuxConnection`] on top. Pairing needs the QUIC exporter, so only paired devices use it.

use link_proto::pairing::{EXPORTER_LABEL, EXPORTER_LEN};
use std::io;
use std::os::fd::{AsFd, OwnedFd};
use std::pin::Pin;
use std::sync::Arc;
use std::task::{Context, Poll, ready};
use std::time::Duration;

use link_proto::STREAM_ALPN;
use rustls::client::AlwaysResolvesClientRawPublicKeys;
use rustls::pki_types::{CertificateDer, ServerName};
use rustls::server::AlwaysResolvesServerRawPublicKeys;
use tokio::io::unix::AsyncFd;
use tokio::io::{AsyncRead, AsyncWrite, ReadBuf};
use tokio_rustls::{TlsAcceptor, TlsConnector};

use crate::Error;
use crate::identity::{Identity, Spki};
use crate::mux::MuxConnection;
use crate::tls::{self, ServerPin};
use crate::transport::tls13_provider;

/// Files are offered over Bluetooth only up to this total; a larger offer waits for Wi-Fi or the hotspot.
pub const BLUETOOTH_FILE_LIMIT: u64 = 20 * 1024 * 1024;

/// A send past this total asks for the phone's hotspot first, since it takes longer over Bluetooth than joining it; a
/// smaller one, or one within [`BLUETOOTH_FILE_LIMIT`] whose hotspot fails, goes over Bluetooth.
pub const BLUETOOTH_UPGRADE_ABOVE: u64 = 1024 * 1024;

/// A Bluetooth handshake crosses a slower link than Wi-Fi, so it gets longer than QUIC's.
pub const HANDSHAKE_TIMEOUT: Duration = Duration::from_secs(15);

/// The phone side.
#[derive(Clone)]
pub struct StreamDialer {
    connector: TlsConnector,
}

/// The desktop side.
#[derive(Clone)]
pub struct StreamAcceptor {
    acceptor: TlsAcceptor,
}

impl StreamDialer {
    pub fn new(identity: &Identity) -> Result<Self, Error> {
        let resolver = AlwaysResolvesClientRawPublicKeys::new(tls::certified_key(identity)?);
        let mut config = rustls::ClientConfig::builder_with_provider(tls13_provider())
            .with_protocol_versions(&[&rustls::version::TLS13])?
            .dangerous()
            .with_custom_certificate_verifier(tls::client_verifier())
            .with_client_cert_resolver(Arc::new(resolver));
        config.alpn_protocols = vec![STREAM_ALPN.to_vec()];
        Ok(Self { connector: TlsConnector::from(Arc::new(config)) })
    }

    /// Authenticates the desktop behind `io` against `pin` and starts the connection; the desktop's key comes back
    /// for the caller to match against the one it meant to reach.
    pub async fn connect<T>(
        &self,
        io: T,
        pin: [u8; 32],
        keep_alive: Option<Duration>,
    ) -> Result<(MuxConnection, Spki), Error>
    where
        T: AsyncRead + AsyncWrite + Unpin + Send + 'static,
    {
        let name = ServerName::try_from(ServerPin::Key(pin).server_name()).map_err(|_| Error::BadKey)?;
        let tls = tokio::time::timeout(HANDSHAKE_TIMEOUT, self.connector.connect(name, io)).await??;
        let peer = spki(tls.get_ref().1.peer_certificates())?;
        let exporter = exporter(tls.get_ref().1)?;
        Ok((MuxConnection::new(tls, true, keep_alive).with_exporter(exporter), peer))
    }
}

impl StreamAcceptor {
    pub fn new(identity: &Identity) -> Result<Self, Error> {
        let resolver = AlwaysResolvesServerRawPublicKeys::new(tls::certified_key(identity)?);
        let mut config = rustls::ServerConfig::builder_with_provider(tls13_provider())
            .with_protocol_versions(&[&rustls::version::TLS13])?
            .with_client_cert_verifier(tls::server_verifier())
            .with_cert_resolver(Arc::new(resolver));
        config.alpn_protocols = vec![STREAM_ALPN.to_vec()];
        Ok(Self { acceptor: TlsAcceptor::from(Arc::new(config)) })
    }

    /// Completes the handshake with the phone behind `io`; its key comes back for admission, as over QUIC.
    pub async fn accept<T>(&self, io: T) -> Result<(MuxConnection, Spki), Error>
    where
        T: AsyncRead + AsyncWrite + Unpin + Send + 'static,
    {
        let tls = tokio::time::timeout(HANDSHAKE_TIMEOUT, self.acceptor.accept(io)).await??;
        let peer = spki(tls.get_ref().1.peer_certificates())?;
        let exporter = exporter(tls.get_ref().1)?;
        Ok((MuxConnection::new(tls, false, None).with_exporter(exporter), peer))
    }
}

/// The same keying material QUIC exports, so pairing runs over Bluetooth as it does over Wi-Fi.
fn exporter<Data>(tls: &rustls::ConnectionCommon<Data>) -> Result<[u8; EXPORTER_LEN], Error> {
    tls.export_keying_material([0; EXPORTER_LEN], EXPORTER_LABEL, Some(&[])).map_err(|_| Error::BadKey)
}

fn spki(certs: Option<&[CertificateDer<'_>]>) -> Result<Spki, Error> {
    Spki::from_der(certs.and_then(<[_]>::first).ok_or(Error::BadKey)?.to_vec())
}

/// Opens a byte stream to a desktop's Bluetooth adapter (`AA:BB:CC:DD:EE:FF`). Blocking, since the platforms connect
/// RFCOMM synchronously; the phone calls it off the runtime. With `pair`, the platform first pairs Bluetooth with the
/// desktop when it is not paired yet, which the user confirms on both screens; only Link pairing asks for that.
pub trait BluetoothOpener: Send + Sync {
    fn open(&self, address: &str, pair: bool) -> io::Result<OwnedFd>;
}

/// A connected stream socket handed over as a descriptor (`BlueZ`'s RFCOMM socket, one end of the Android app's socket
/// pair), read and written without blocking.
pub struct FdStream {
    fd: AsyncFd<OwnedFd>,
}

impl FdStream {
    pub fn new(fd: OwnedFd) -> io::Result<Self> {
        rustix::io::ioctl_fionbio(&fd, true)?;
        Ok(Self { fd: AsyncFd::new(fd)? })
    }
}

impl AsyncRead for FdStream {
    fn poll_read(self: Pin<&mut Self>, cx: &mut Context<'_>, buf: &mut ReadBuf<'_>) -> Poll<io::Result<()>> {
        loop {
            let mut ready = ready!(self.fd.poll_read_ready(cx))?;
            let unfilled = buf.initialize_unfilled();
            match ready.try_io(|fd| rustix::io::read(fd.get_ref().as_fd(), &mut *unfilled).map_err(io::Error::from)) {
                Ok(Ok(read)) => {
                    buf.advance(read);
                    return Poll::Ready(Ok(()));
                }
                Ok(Err(error)) => return Poll::Ready(Err(error)),
                Err(_would_block) => {}
            }
        }
    }
}

impl AsyncWrite for FdStream {
    fn poll_write(self: Pin<&mut Self>, cx: &mut Context<'_>, bytes: &[u8]) -> Poll<io::Result<usize>> {
        loop {
            let mut ready = ready!(self.fd.poll_write_ready(cx))?;
            match ready.try_io(|fd| rustix::io::write(fd.get_ref().as_fd(), bytes).map_err(io::Error::from)) {
                Ok(result) => return Poll::Ready(result),
                Err(_would_block) => {}
            }
        }
    }

    fn poll_flush(self: Pin<&mut Self>, _: &mut Context<'_>) -> Poll<io::Result<()>> {
        Poll::Ready(Ok(()))
    }

    fn poll_shutdown(self: Pin<&mut Self>, _: &mut Context<'_>) -> Poll<io::Result<()>> {
        Poll::Ready(rustix::net::shutdown(self.fd.get_ref(), rustix::net::Shutdown::Write).map_err(io::Error::from))
    }
}
