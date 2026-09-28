//! `umbriel-linkd`: the desktop side of Link. No UI; the shell drives it over D-Bus `org.umbriel.Link1`.

mod bluetooth;
mod browse;
mod dbus;
mod desktop_media;
mod hotspot;
mod hub;
mod listener;
mod localsend;
mod media;
mod mpris;
mod notifications;
mod paths;
mod quickshare;

use std::net::{Ipv6Addr, SocketAddr};

use anyhow::Context;
use link_core::identity::Identity;
use link_core::inbox::Inbox;
use link_core::store::Store;
use link_core::stream::StreamAcceptor;
use link_core::transport::Puncher;
use link_core::{transfer, transport};
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
    let (endpoint, puncher) = bind(&identity)?;
    let port = endpoint.local_addr()?.port();
    if store.port != port {
        store.port = port;
        store.save(&paths.devices)?;
    }
    let bus = zbus::Connection::session().await.context("connecting to the session bus")?;
    let name = dbus::device_name().await;
    log::info!("{} listening on port {port} as {:?}", identity.device_id(), name);

    let state_dir = paths.state.clone();
    let inbox = Inbox::new(paths.downloads.clone(), &paths.state).context("opening the transfer state")?;
    let (transfers, transfer_actor, transfer_events) = transfer::transfers(inbox);
    let (signals, localsend_signals) = tokio::sync::mpsc::unbounded_channel();
    let (localsend_actor, localsend, nearby) =
        localsend::LocalSend::new(&paths.state, paths.downloads.clone(), name.clone(), signals)?;
    let transfers = hub::Transfers { handle: transfers, events: transfer_events, localsend, localsend_signals };
    let (desktop_media, media_requests) = desktop_media::channel();
    let (hub, handle, snapshots, events) =
        hub::Hub::new(identity.spki().clone(), store, paths, desktop_media, transfers, puncher);
    dbus::serve(&bus, handle.clone(), snapshots.clone(), nearby.clone()).await?;
    let (quick_share, qs_handle, qs_watches, qs_events) = quickshare::QuickShare::new(&name, &state_dir)?;
    quickshare::serve(&bus, qs_handle, qs_watches.clone(), name.clone()).await?;
    let bluetooth = bluetooth::Bluetooth::start().await;
    let acceptor = StreamAcceptor::new(&identity)?;
    let listener =
        listener::Listener::new(endpoint.clone(), bluetooth, acceptor, handle.clone(), identity.spki().clone(), name);
    let mut terminate = signal(SignalKind::terminate())?;
    let result = tokio::select! {
        result = hub.run() => result,
        () = transfer_actor.run() => Ok(()),
        result = listener.run() => result,
        result = dbus::forward(&bus, snapshots, events, nearby) => result,
        () = localsend_actor.run() => Ok(()),
        result = quick_share.run() => result,
        result = quickshare::forward(&bus, qs_watches, qs_events) => result,
        result = desktop_media::run(bus.clone(), handle.clone(), media_requests) => result,
        _ = terminate.recv() => Ok(()),
        _ = tokio::signal::ctrl_c() => Ok(()),
    };
    endpoint.close(quinn::VarInt::from_u32(0), b"shutdown");
    endpoint.wait_idle().await;
    result
}

/// Unassigned at IANA; the ufw profile `Umbriel Link` opens it.
const DEFAULT_PORT: u16 = 4717;

/// Binds [`DEFAULT_PORT`], the one the firewall opens; a taken port falls back to a random one for this run only, so
/// the next start returns to the default.
fn bind(identity: &Identity) -> anyhow::Result<(quinn::Endpoint, Puncher)> {
    let addr = |port| SocketAddr::from((Ipv6Addr::UNSPECIFIED, port));
    transport::punchable_server_endpoint(identity, addr(DEFAULT_PORT))
        .or_else(|error| {
            log::warn!("port {DEFAULT_PORT}: {error}; choosing another");
            transport::punchable_server_endpoint(identity, addr(0))
        })
        .context("binding the QUIC endpoint")
}
