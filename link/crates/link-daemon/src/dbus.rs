//! `org.umbriel.Link1` on the session bus; contract in `protocol/link-v1/org.umbriel.Link1.xml`.
#![expect(clippy::too_many_arguments, reason = "the NotificationPosted signal's arguments are the D-Bus contract")]

use link_core::identity::DeviceId;
use link_core::proto::message::{
    MAX_NAME_LEN, Message, NotificationAction, NotificationDismiss, NotificationPosted, Share, ShareKind,
};
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

#[derive(Debug, zbus::DBusError)]
#[zbus(prefix = "org.umbriel.Link1.Error")]
enum LinkError {
    #[zbus(error)]
    ZBus(zbus::Error),
    NotConnected(String),
    Rejected(String),
    Failed(String),
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

    /// Returns once the device acknowledged the share.
    async fn share(&self, device_id: String, kind: String, text: String) -> Result<(), LinkError> {
        let id = DeviceId::parse(&device_id).map_err(|_| LinkError::Rejected("not a device id".to_owned()))?;
        let kind = ShareKind::parse(&kind).ok_or_else(|| LinkError::Rejected("kind is text or link".to_owned()))?;
        let share = Share { kind, text };
        share.check().map_err(|rejected| LinkError::Rejected(rejected.to_string()))?;
        let Some(session) = self.hub.session(id.clone()).await else {
            return Err(LinkError::NotConnected(format!("{id} is not connected")));
        };
        session.share(share).await.map_err(|error| LinkError::Failed(error.to_string()))
    }

    /// Runs a phone notification's action; `reply_text` is empty for an action that takes none.
    async fn notification_action(
        &self,
        device_id: String,
        id: String,
        action: String,
        reply_text: String,
    ) -> Result<(), LinkError> {
        let reply_text = Some(reply_text).filter(|text| !text.is_empty());
        self.send(&device_id, Message::NotificationAction(NotificationAction { id, action, reply_text })).await
    }

    async fn notification_dismiss(&self, device_id: String, id: String) -> Result<(), LinkError> {
        self.send(&device_id, Message::NotificationDismiss(NotificationDismiss { id })).await
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

    #[zbus(signal)]
    async fn received(emitter: &SignalEmitter<'_>, device_id: &str, kind: &str, text: &str) -> zbus::Result<()>;

    #[zbus(signal)]
    async fn notification_posted(
        emitter: &SignalEmitter<'_>,
        device_id: &str,
        id: &str,
        app: &str,
        title: &str,
        text: &str,
        icon: &[u8],
        actions: Vec<(String, String, bool)>,
    ) -> zbus::Result<()>;

    #[zbus(signal)]
    async fn notification_removed(emitter: &SignalEmitter<'_>, device_id: &str, id: &str) -> zbus::Result<()>;
}

impl Link {
    /// Sends an unacknowledged message to a connected device.
    async fn send(&self, device_id: &str, message: Message) -> Result<(), LinkError> {
        let id = DeviceId::parse(device_id).map_err(|_| LinkError::Rejected("not a device id".to_owned()))?;
        message.validate().map_err(|error| LinkError::Rejected(error.to_string()))?;
        let Some(session) = self.hub.session(id.clone()).await else {
            return Err(LinkError::NotConnected(format!("{id} is not connected")));
        };
        session.send(message).await.map_err(|error| LinkError::Failed(error.to_string()))
    }
}

async fn emit_posted(emitter: &SignalEmitter<'_>, id: &DeviceId, posted: NotificationPosted) -> zbus::Result<()> {
    let actions = posted.actions.into_iter().map(|action| (action.id, action.label, action.reply)).collect();
    let icon = posted.icon.unwrap_or_default();
    Link::notification_posted(
        emitter,
        id.as_str(),
        &posted.id,
        &posted.app,
        &posted.title,
        &posted.text,
        &icon,
        actions,
    )
    .await
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
                Event::Received { id, share } => {
                    Link::received(emitter, id.as_str(), share.kind.as_str(), &share.text).await?;
                }
                Event::NotificationPosted { id, posted } => emit_posted(emitter, &id, posted).await?,
                Event::NotificationRemoved { id, notification } => {
                    Link::notification_removed(emitter, id.as_str(), &notification).await?;
                }
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
