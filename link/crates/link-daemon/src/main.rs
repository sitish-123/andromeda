//! `umbriel-linkd`: the desktop side of Link. No UI; the shell drives it over D-Bus `org.umbriel.Link1`.

mod dbus;
mod hub;
mod listener;
mod paths;

use std::net::{Ipv6Addr, SocketAddr};

use anyhow::Context;
use link_core::identity::Identity;
use link_core::store::Store;
use link_core::transport;
use tokio::signal::unix::{SignalKind, signal};

fn main() -> anyhow::Result<()> {
    env_logger::Builder::from_env(env_logger::Env::default().default_filter_or("info,zbus=warn,tracing=warn"))
        .format_timestamp(None)
        .init();
    tokio::runtime::Builder::new_current_thread().enable_all().build()?.block_on(run())
}

async fn run() -> anyhow::Result<()> {
    let paths = paths::Paths::create()?;
    let identity = Identity::load_or_create(&paths.identity).context("loading the device key")?;
    let mut store = Store::load(&paths.devices).context("loading the device store")?;
    let endpoint = bind(&identity, store.port)?;
    let port = endpoint.local_addr()?.port();
    if store.port != port {
        store.port = port;
        store.save(&paths.devices)?;
    }
    let bus = zbus::Connection::session().await.context("connecting to the session bus")?;
    let name = dbus::device_name().await;
    log::info!("{} listening on port {port} as {:?}", identity.device_id(), name);

    let (hub, handle, snapshots, events) = hub::Hub::new(identity.spki().clone(), store, paths);
    dbus::serve(&bus, handle.clone(), snapshots.clone()).await?;
    let listener = listener::Listener::new(endpoint.clone(), handle, identity.spki().clone(), name);
    let mut terminate = signal(SignalKind::terminate())?;
    let result = tokio::select! {
        result = hub.run() => result,
        result = listener.run() => result,
        result = dbus::forward(&bus, snapshots, events) => result,
        _ = terminate.recv() => Ok(()),
        _ = tokio::signal::ctrl_c() => Ok(()),
    };
    endpoint.close(quinn::VarInt::from_u32(0), b"shutdown");
    endpoint.wait_idle().await;
    result
}

/// Unassigned at IANA; the ufw profile `Umbriel Link` opens it.
const DEFAULT_PORT: u16 = 4717;

/// Binds the stored port so last-known addresses survive restarts, or [`DEFAULT_PORT`] in a new store; a taken port
/// falls back to a random one.
fn bind(identity: &Identity, stored: u16) -> anyhow::Result<quinn::Endpoint> {
    let port = if stored == 0 { DEFAULT_PORT } else { stored };
    let addr = |port| SocketAddr::from((Ipv6Addr::UNSPECIFIED, port));
    transport::server_endpoint(identity, addr(port))
        .or_else(|error| {
            log::warn!("port {port}: {error}; choosing another");
            transport::server_endpoint(identity, addr(0))
        })
        .context("binding the QUIC endpoint")
}
