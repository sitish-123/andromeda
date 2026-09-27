//! `org.umbriel.Link1` on the session bus; contract in `protocol/link-v1/org.umbriel.Link1.xml`.

use link_core::identity::DeviceId;
use link_core::proto::message::MAX_NAME_LEN;
use tokio::sync::{mpsc, watch};
use zbus::fdo;
use zbus::object_server::SignalEmitter;

use crate::hub::{Event, HubHandle, Snapshot};

const PATH: &str = "/org/umbriel/Link1";
const NAME: &str = "org.umbriel.Link1";

struct Link {
    hub: HubHandle,
    snapshots: watch::Receiver<Snapshot>,
}

#[zbus::interface(name = "org.umbriel.Link1")]
impl Link {
    async fn start_pairing(&self) -> fdo::Result<(String, String)> {
        self.hub.start_pairing().await.map_err(|error| fdo::Error::Failed(error.to_string()))
    }

    async fn cancel_pairing(&self) {
        self.hub.cancel_pairing().await;
    }

    async fn unpair(&self, device_id: String) -> fdo::Result<()> {
        let id = DeviceId::parse(&device_id).map_err(|_| fdo::Error::InvalidArgs("not a device id".to_owned()))?;
        if self.hub.unpair(id).await { Ok(()) } else { Err(fdo::Error::InvalidArgs("unknown device".to_owned())) }
    }

    #[zbus(property)]
    fn devices(&self) -> Vec<(String, String, bool)> {
        self.snapshots.borrow().devices.clone()
    }

    #[zbus(property)]
    fn pairing(&self) -> bool {
        self.snapshots.borrow().pairing
    }

    #[zbus(signal)]
    async fn pairing_finished(emitter: &SignalEmitter<'_>, device_id: &str, name: &str) -> zbus::Result<()>;

    #[zbus(signal)]
    async fn pairing_failed(emitter: &SignalEmitter<'_>, reason: &str) -> zbus::Result<()>;
}

pub async fn serve(bus: &zbus::Connection, hub: HubHandle, snapshots: watch::Receiver<Snapshot>) -> anyhow::Result<()> {
    bus.object_server().at(PATH, Link { hub, snapshots }).await?;
    bus.request_name(NAME).await?;
    Ok(())
}

/// Turns hub snapshots into `PropertiesChanged` and hub events into signals.
pub async fn forward(
    bus: &zbus::Connection,
    mut snapshots: watch::Receiver<Snapshot>,
    mut events: mpsc::Receiver<Event>,
) -> anyhow::Result<()> {
    let link = bus.object_server().interface::<_, Link>(PATH).await?;
    let emitter = link.signal_emitter();
    let mut last = snapshots.borrow_and_update().clone();
    loop {
        tokio::select! {
            changed = snapshots.changed() => {
                changed?;
                let next = snapshots.borrow_and_update().clone();
                if next.devices != last.devices {
                    link.get().await.devices_changed(emitter).await?;
                }
                if next.pairing != last.pairing {
                    link.get().await.pairing_changed(emitter).await?;
                }
                last = next;
            }
            Some(event) = events.recv() => match event {
                Event::PairingFinished { id, name } => Link::pairing_finished(emitter, id.as_str(), &name).await?,
                Event::PairingFailed { reason } => Link::pairing_failed(emitter, &reason).await?,
            },
        }
    }
}

/// The name other devices see: hostnamed's pretty name, else the kernel hostname.
pub async fn device_name() -> String {
    let pretty = async {
        let system = zbus::Connection::system().await?;
        let proxy = zbus::Proxy::new(
            &system,
            "org.freedesktop.hostname1",
            "/org/freedesktop/hostname1",
            "org.freedesktop.hostname1",
        )
        .await?;
        proxy.get_property::<String>("PrettyHostname").await
    };
    let name = match pretty.await {
        Ok(name) if !name.trim().is_empty() => name,
        _ => std::fs::read_to_string("/proc/sys/kernel/hostname").unwrap_or_default(),
    };
    let name: String = name.trim().chars().take(MAX_NAME_LEN).collect();
    if name.is_empty() { "Linux desktop".to_owned() } else { name }
}
