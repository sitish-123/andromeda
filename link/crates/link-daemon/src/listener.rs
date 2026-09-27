//! Accepts QUIC connections and runs each one as a pairing attempt or a session, as the hub admits it.

use std::sync::Arc;

use link_core::control::Control;
use link_core::identity::Spki;
use link_core::pairing::pair_as_server;
use link_core::proto::message::{Hello, Message};
use link_core::proto::pairing::Secrets;
use link_core::proto::{CloseCode, VERSION};
use link_core::transport::CONNECT_TIMEOUT;
use link_core::{Error, close, close_code_for, net, tls};
use tokio::task::JoinSet;

use crate::hub::{Admission, HubHandle};

pub struct Listener {
    endpoint: quinn::Endpoint,
    context: Arc<Context>,
}

struct Context {
    hub: HubHandle,
    own: Spki,
    name: String,
    port: u16,
}

impl Listener {
    pub fn new(endpoint: quinn::Endpoint, hub: HubHandle, own: Spki, name: String) -> Self {
        let port = endpoint.local_addr().map_or(0, |addr| addr.port());
        Self { endpoint, context: Arc::new(Context { hub, own, name, port }) }
    }

    pub async fn run(self) -> anyhow::Result<()> {
        let mut connections = JoinSet::new();
        loop {
            tokio::select! {
                incoming = self.endpoint.accept() => {
                    let Some(incoming) = incoming else { return Ok(()) };
                    connections.spawn(handle(incoming, self.context.clone()));
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
        return close(&connection, CloseCode::ProtocolError);
    };
    let result = match context.hub.admit(peer.clone()).await {
        Admission::Reject(code) => {
            log::info!("{remote}: {} rejected: {}", peer.device_id(), code.reason());
            close(&connection, code);
            return;
        }
        Admission::Session => session(&connection, &peer, &context).await,
        Admission::Pair(secrets) => pairing(&connection, &peer, &secrets, &context).await,
    };
    let id = peer.device_id();
    context.hub.disconnected(id.clone(), connection.stable_id()).await;
    match result {
        Ok(()) | Err(Error::Timeout | Error::Closed(_)) => log::info!("{id}: disconnected"),
        Err(error) => {
            log::info!("{id}: {error}");
            close(&connection, close_code_for(&error));
        }
    }
}

async fn pairing(
    connection: &quinn::Connection,
    peer: &Spki,
    secrets: &Secrets,
    context: &Context,
) -> Result<(), Error> {
    let attempt = async {
        let mut control = Control::accept(connection, None).await?;
        let hello = control.hello_as_server(context.hello()).await?;
        pair_as_server(connection, &mut control, secrets, &context.own, peer).await?;
        Ok::<_, Error>((control, hello))
    };
    match attempt.await {
        Ok((control, hello)) => {
            context.hub.paired(peer.clone(), hello.name.clone()).await;
            serve(connection, peer, control, hello, context).await
        }
        Err(error) => {
            context.hub.pairing_failed(error.to_string()).await;
            Err(error)
        }
    }
}

async fn session(connection: &quinn::Connection, peer: &Spki, context: &Context) -> Result<(), Error> {
    let mut control = Control::accept(connection, None).await?;
    let hello = control.hello_as_server(context.hello()).await?;
    serve(connection, peer, control, hello, context).await
}

/// A live session: waits for control messages until the peer leaves or the connection idles out.
async fn serve(
    connection: &quinn::Connection,
    peer: &Spki,
    mut control: Control,
    hello: Hello,
    context: &Context,
) -> Result<(), Error> {
    let id = peer.device_id();
    context.hub.connected(id.clone(), hello.name, connection.clone()).await;
    match control.recv_idle().await? {
        Message::Unpair => {
            context.hub.peer_unpaired(id).await;
            close(connection, CloseCode::Done);
            Ok(())
        }
        other => Err(Error::Unexpected(other.kind())),
    }
}

impl Context {
    fn hello(&self) -> Hello {
        let addresses = net::local_addresses(self.port).iter().map(ToString::to_string).collect();
        Hello { version: VERSION, name: self.name.clone(), addresses }
    }
}
