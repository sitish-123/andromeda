//! Accepts QUIC connections and runs each one as a pairing attempt or a session, as the hub admits it; Bluetooth
//! connections run as sessions only, since pairing binds to the QUIC handshake.

use std::os::fd::OwnedFd;
use std::sync::Arc;

use link_core::control::Control;
use link_core::identity::Spki;
use link_core::pairing::pair_as_server;
use link_core::proto::message::BLUETOOTH_SCHEME;
use link_core::proto::message::Hello;
use link_core::proto::session::Role;
use link_core::proto::{CloseCode, VERSION};
use link_core::session::session as start_session;
use link_core::stream::{FdStream, StreamAcceptor};
use link_core::transport::CONNECT_TIMEOUT;
use link_core::wire::Connection;
use link_core::{Error, close_code_for, net, tls};
use tokio::task::JoinSet;

use crate::bluetooth::Bluetooth;

use crate::hub::{Admission, Attempt, HubHandle};

pub struct Listener {
    endpoint: quinn::Endpoint,
    /// Held whole: the RFCOMM profile lives as long as its system bus connection inside.
    bluetooth: Bluetooth,
    acceptor: StreamAcceptor,
    context: Arc<Context>,
}

struct Context {
    hub: HubHandle,
    own: Spki,
    name: String,
    port: u16,
    bluetooth: Option<String>,
}

impl Listener {
    pub fn new(
        endpoint: quinn::Endpoint,
        bluetooth: Bluetooth,
        acceptor: StreamAcceptor,
        hub: HubHandle,
        own: Spki,
        name: String,
    ) -> Self {
        let port = endpoint.local_addr().map_or(0, |addr| addr.port());
        let context = Arc::new(Context { hub, own, name, port, bluetooth: bluetooth.address.clone() });
        Self { endpoint, bluetooth, acceptor, context }
    }

    pub async fn run(mut self) -> anyhow::Result<()> {
        let mut connections = JoinSet::new();
        loop {
            tokio::select! {
                incoming = self.endpoint.accept() => {
                    let Some(incoming) = incoming else { return Ok(()) };
                    connections.spawn(handle(incoming, self.context.clone()));
                }
                Some(fd) = self.bluetooth.incoming.recv() => {
                    connections.spawn(handle_stream(fd, self.acceptor.clone(), self.context.clone()));
                }
                Some(joined) = connections.join_next() => {
                    if let Err(join) = joined {
                        log::error!("connection task: {join}");
                    }
                }
            }
        }
    }
}

async fn handle(incoming: quinn::Incoming, context: Arc<Context>) {
    let remote = incoming.remote_address();
    let connection = match tokio::time::timeout(CONNECT_TIMEOUT, incoming).await {
        Ok(Ok(connection)) => connection,
        Ok(Err(error)) => return log::info!("{remote}: handshake failed: {error}"),
        Err(_) => return log::info!("{remote}: handshake timed out"),
    };
    let Ok(peer) = tls::peer_spki(connection.peer_identity()) else {
        return Connection::from(connection).close(CloseCode::ProtocolError);
    };
    let quic = connection;
    let connection = Connection::from(quic.clone());
    let result = match context.hub.admit(peer.clone()).await {
        Admission::Reject(code) => {
            log::info!("{remote}: {} rejected: {}", peer.device_id(), code.reason());
            connection.close(code);
            return;
        }
        Admission::Session => session(&connection, &peer, &context).await,
        Admission::Pair(attempt) => pairing(&quic, &connection, &peer, &attempt, &context).await,
    };
    ended(&connection, &peer, result, &context).await;
}

/// A Bluetooth connection: the TLS handshake over the stream, then a session as over QUIC.
async fn handle_stream(fd: OwnedFd, acceptor: StreamAcceptor, context: Arc<Context>) {
    let accepted = match FdStream::new(fd) {
        Ok(stream) => acceptor.accept(stream).await,
        Err(error) => Err(error.into()),
    };
    let (mux, peer) = match accepted {
        Ok(accepted) => accepted,
        Err(error) => return log::info!("Bluetooth: handshake failed: {error}"),
    };
    let connection = Connection::Stream(mux);
    let result = match context.hub.admit(peer.clone()).await {
        Admission::Session => session(&connection, &peer, &context).await,
        Admission::Reject(code) => {
            log::info!("Bluetooth: {} rejected: {}", peer.device_id(), code.reason());
            return connection.close(code);
        }
        Admission::Pair(_) => {
            log::info!("Bluetooth: {} tried to pair, which needs Wi-Fi", peer.device_id());
            return connection.close(CloseCode::NotPaired);
        }
    };
    ended(&connection, &peer, result, &context).await;
}

async fn ended(connection: &Connection, peer: &Spki, result: Result<(), Error>, context: &Context) {
    let id = peer.device_id();
    context.hub.disconnected(id.clone(), connection.stable_id()).await;
    match result {
        Ok(()) | Err(Error::Timeout | Error::Closed(_)) => log::info!("{id}: disconnected"),
        Err(error) => {
            log::info!("{id}: {error}");
            connection.close(close_code_for(&error));
        }
    }
}

async fn pairing(
    quic: &quinn::Connection,
    connection: &Connection,
    peer: &Spki,
    attempt: &Attempt,
    context: &Context,
) -> Result<(), Error> {
    let handshake = async {
        let mut control = Control::accept(connection, None).await?;
        let hello = control.hello_as_server(context.hello()).await?;
        pair_as_server(quic, &mut control, &attempt.secrets, &context.own, peer).await?;
        Ok::<_, Error>((control, hello))
    };
    match handshake.await {
        Ok((control, hello)) => {
            context.hub.paired(peer.clone(), hello.name.clone(), attempt.window).await;
            serve(connection, peer, control, hello, context).await
        }
        Err(error) => {
            context.hub.pairing_failed(error.to_string(), attempt.window).await;
            Err(error)
        }
    }
}

async fn session(connection: &Connection, peer: &Spki, context: &Context) -> Result<(), Error> {
    let mut control = Control::accept(connection, None).await?;
    let hello = control.hello_as_server(context.hello()).await?;
    serve(connection, peer, control, hello, context).await
}

/// A live session: this connection's task runs its session actor until the peer leaves or the connection idles out.
async fn serve(
    connection: &Connection,
    peer: &Spki,
    control: Control,
    hello: Hello,
    context: &Context,
) -> Result<(), Error> {
    let id = peer.device_id();
    let (handle, actor) = start_session(connection.clone(), control, Role::Desktop, context.hub.route(id.clone()));
    context.hub.connected(id, hello.name, handle).await;
    actor.run().await
}

impl Context {
    fn hello(&self) -> Hello {
        let mut addresses: Vec<String> = net::local_addresses(self.port).iter().map(ToString::to_string).collect();
        addresses.extend(self.bluetooth.iter().map(|address| format!("{BLUETOOTH_SCHEME}{address}")));
        Hello { version: VERSION, name: self.name.clone(), addresses }
    }
}
