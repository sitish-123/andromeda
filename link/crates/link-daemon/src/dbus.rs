//! `org.umbriel.Link1` on the session bus; contract in `protocol/link-v1/org.umbriel.Link1.xml`.
#![expect(clippy::too_many_arguments, reason = "the NotificationPosted signal's arguments are the D-Bus contract")]

use std::collections::HashMap;
use std::fs::File;
use std::io::Read as _;
use std::os::fd::OwnedFd;

use link_core::Error;
use link_core::identity::DeviceId;
use link_core::proto::message::{
    CallAction, CallActionKind, MAX_NAME_LEN, MAX_SHARE_LEN, Message, MirrorAction, MirrorInput, NotificationAction,
    NotificationDismiss, NotificationPosted, Ring, Ringing, Share, ShareKind, TransferId, is_text_mime,
};
use link_core::transfer::{LocalClip, Source};
use tokio::sync::{mpsc, watch};
use zbus::fdo;
use zbus::object_server::SignalEmitter;

use crate::browse;
use crate::hub::{Event, HubHandle, Signal, Snapshot};

pub const PATH: &str = "/org/umbriel/Link1";
const NAME: &str = "org.umbriel.Link1";
/// The shell passes descriptors, not types; the receiver's platform types files by name.
const OCTET_STREAM: &str = "application/octet-stream";

struct Link {
    hub: HubHandle,
    snapshots: watch::Receiver<Snapshot>,
    nearby: watch::Receiver<Vec<(String, String)>>,
}

#[derive(Debug, zbus::DBusError)]
#[zbus(prefix = "org.umbriel.Link1.Error")]
enum LinkError {
    #[zbus(error)]
    ZBus(zbus::Error),
    NotConnected(String),
    Rejected(String),
    Failed(String),
    /// The phone refused a browse request; the message is its reason (`not-found`, `not-allowed`, ...).
    Refused(String),
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

    /// Offers the files behind the descriptors to a connected device and returns the transfer id at once; the
    /// transfer signals report the rest.
    async fn send_files(
        &self,
        device_id: String,
        files: Vec<(zbus::zvariant::OwnedFd, String)>,
    ) -> Result<String, LinkError> {
        if device_id.strip_prefix(crate::localsend::PREFIX).is_none() && DeviceId::parse(&device_id).is_err() {
            return Err(LinkError::Rejected("not a device id".to_owned()));
        }
        if files.is_empty() {
            return Err(LinkError::Rejected("no files".to_owned()));
        }
        let sources = files
            .into_iter()
            .map(|(fd, name)| Source::new(File::from(OwnedFd::from(fd)), name, OCTET_STREAM.to_owned()))
            .collect::<Result<Vec<_>, _>>()
            .map_err(|error| LinkError::Rejected(error.to_string()))?;
        match self.hub.send_files(device_id.clone(), sources).await {
            Ok(transfer) => Ok(transfer.to_hex()),
            Err(Error::NotConnected) => Err(LinkError::NotConnected(format!("{device_id} is not connected"))),
            Err(error) => Err(LinkError::Failed(error.to_string())),
        }
    }

    async fn accept_transfer(&self, transfer_id: String) -> Result<(), LinkError> {
        self.decide(&transfer_id, true).await
    }

    async fn decline_transfer(&self, transfer_id: String) -> Result<(), LinkError> {
        self.decide(&transfer_id, false).await
    }

    async fn cancel_transfer(&self, transfer_id: String) -> Result<(), LinkError> {
        let id = parse_transfer(&transfer_id)?;
        if self.hub.cancel_transfer(id).await {
            Ok(())
        } else {
            Err(LinkError::Rejected(format!("no open transfer {id}")))
        }
    }

    async fn set_auto_accept(&self, device_id: String, enabled: bool) -> fdo::Result<()> {
        let id = DeviceId::parse(&device_id).map_err(|_| fdo::Error::InvalidArgs("not a device id".to_owned()))?;
        if self.hub.set_auto_accept(id, enabled).await {
            Ok(())
        } else {
            Err(fdo::Error::InvalidArgs("unknown device".to_owned()))
        }
    }

    async fn set_grant(&self, device_id: String, feature: String, granted: bool) -> fdo::Result<()> {
        let id = DeviceId::parse(&device_id).map_err(|_| fdo::Error::InvalidArgs("not a device id".to_owned()))?;
        if self.hub.set_grant(id, feature, granted).await {
            Ok(())
        } else {
            Err(fdo::Error::InvalidArgs("unknown device or feature".to_owned()))
        }
    }

    /// Offers the desktop's new clipboard to connected devices holding the clipboard grant. `data` holds the first
    /// type's bytes; text is sent inline, anything else only when a device pulls it.
    async fn offer_clipboard(&self, mimes: Vec<String>, data: zbus::zvariant::OwnedFd) -> Result<(), LinkError> {
        let file = File::from(OwnedFd::from(data));
        let size = file.metadata().map_err(|error| LinkError::Rejected(error.to_string()))?.len();
        let text = if mimes.first().is_some_and(|mime| is_text_mime(mime)) && size <= MAX_SHARE_LEN as u64 {
            let mut text = String::new();
            (&file).read_to_string(&mut text).map_err(|error| LinkError::Rejected(error.to_string()))?;
            Some(text).filter(|text| !text.is_empty())
        } else {
            None
        };
        let clip = LocalClip { mimes, text, data: Some(file) };
        let peers = self.hub.clipboard_peers().await;
        self.hub.transfers().offer_clip(peers, clip).await.map_err(|error| LinkError::Rejected(error.to_string()))
    }

    /// Writes a device's offered clip into `sink` (the write end a paste target reads) and returns the byte count.
    async fn pull_clipboard(
        &self,
        device_id: String,
        id: u64,
        mime: String,
        sink: zbus::zvariant::OwnedFd,
    ) -> Result<u64, LinkError> {
        let peer = DeviceId::parse(&device_id).map_err(|_| LinkError::Rejected("not a device id".to_owned()))?;
        let sink = File::from(OwnedFd::from(sink));
        match self.hub.transfers().pull_clip(peer, id, mime, sink).await {
            Ok(bytes) => Ok(bytes),
            Err(Error::NotConnected) => Err(LinkError::NotConnected(format!("{device_id} is not connected"))),
            Err(error) => Err(LinkError::Failed(error.to_string())),
        }
    }

    /// Connected device id to `(battery percent, charging, network)`; network is wifi, cellular, ethernet, none, or
    /// other.
    #[zbus(property)]
    fn device_status(&self) -> HashMap<String, (u32, bool, String)> {
        self.snapshots.borrow().status.iter().cloned().collect()
    }

    /// Turns the LocalSend backend on or off; kept across restarts.
    async fn set_local_send_visible(&self, visible: bool) {
        self.hub.set_localsend(visible).await;
    }

    #[zbus(property)]
    fn local_send_visible(&self) -> bool {
        self.snapshots.borrow().localsend
    }

    /// LocalSend devices on the LAN, `(localsend:<fingerprint>, alias)`, while the backend is on.
    #[zbus(property)]
    fn nearby(&self) -> Vec<(String, String)> {
        self.nearby.borrow().clone()
    }

    #[zbus(property)]
    fn grants(&self) -> HashMap<String, Vec<String>> {
        self.snapshots.borrow().grants.iter().cloned().collect()
    }

    #[zbus(signal)]
    async fn clipboard_offered(
        emitter: &SignalEmitter<'_>,
        device_id: &str,
        id: u64,
        mimes: Vec<String>,
        size: u64,
    ) -> zbus::Result<()>;

    #[zbus(property)]
    fn auto_accept(&self) -> Vec<String> {
        self.snapshots.borrow().auto_accept.clone()
    }

    #[zbus(signal)]
    async fn transfer_offered(
        emitter: &SignalEmitter<'_>,
        transfer_id: &str,
        device_id: &str,
        files: Vec<(String, u64)>,
    ) -> zbus::Result<()>;

    #[zbus(signal)]
    async fn transfer_progress(
        emitter: &SignalEmitter<'_>,
        transfer_id: &str,
        bytes: u64,
        total: u64,
    ) -> zbus::Result<()>;

    #[zbus(signal)]
    async fn transfer_finished(
        emitter: &SignalEmitter<'_>,
        transfer_id: &str,
        status: &str,
        paths: Vec<String>,
    ) -> zbus::Result<()>;

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

    /// Lists a folder on a connected phone whose browse switch for this desktop is on: `/` for its roots, else
    /// `/<root>/...`. Each entry is (name, whether a folder, size, mtime in Unix seconds).
    async fn list_files(&self, device_id: String, path: String) -> Result<Vec<(String, bool, u64, u64)>, LinkError> {
        if !link_core::proto::message::fs_path_valid(&path) {
            return Err(LinkError::Rejected("not a browse path".to_owned()));
        }
        match self.browse(&device_id, browse::Request::List { path }).await? {
            browse::Answer::Entries(entries) => {
                Ok(entries.into_iter().map(|entry| (entry.name, entry.dir, entry.size, entry.mtime)).collect())
            }
            browse::Answer::Data(_) => Err(LinkError::Failed("the phone answered a listing with data".to_owned())),
        }
    }

    /// Up to `length` bytes (at most 1 MiB) of a file on a connected phone, from `offset`; fewer at its end.
    async fn read_file(&self, device_id: String, path: String, offset: u64, length: u32) -> Result<Vec<u8>, LinkError> {
        if !link_core::proto::message::fs_path_valid(&path)
            || !(1..=link_core::proto::message::MAX_FS_READ).contains(&length)
        {
            return Err(LinkError::Rejected("not a browse path, or a length outside 1 byte to 1 MiB".to_owned()));
        }
        match self.browse(&device_id, browse::Request::Read { path, offset, len: length }).await? {
            browse::Answer::Data(data) => Ok(data),
            browse::Answer::Entries(_) => Err(LinkError::Failed("the phone answered a read with a listing".to_owned())),
        }
    }

    /// Asks the phone to mirror its screen and returns the viewer's end of the video, with its size, once the user
    /// allowed it there (up to 60 s). The socket carries the phone's access units in the stream's framing.
    async fn open_mirror(&self, device_id: String) -> Result<(zbus::zvariant::OwnedFd, u32, u32), LinkError> {
        use crate::mirror::Failure;
        let id = DeviceId::parse(&device_id).map_err(|_| LinkError::Rejected("not a device id".to_owned()))?;
        match self.hub.open_mirror(id).await {
            Ok(opened) => Ok((opened.stream.into(), opened.width, opened.height)),
            Err(Failure::NotConnected) => Err(LinkError::NotConnected(format!("{device_id} is not connected"))),
            Err(Failure::NeedsWifi) => Err(LinkError::Failed("mirroring needs Wi-Fi, not Bluetooth".to_owned())),
            Err(Failure::Busy) => Err(LinkError::Failed("that phone is already mirroring".to_owned())),
            Err(Failure::Refused(reason)) => Err(LinkError::Refused(reason)),
            Err(Failure::TimedOut) => Err(LinkError::Failed("nobody allowed it on the phone in time".to_owned())),
            Err(Failure::Failed(reason)) => Err(LinkError::Failed(reason)),
        }
    }

    /// Input on the mirrored screen: `action` is tap, long-press, swipe, scroll, back, home, recents, or text;
    /// `args` holds x, y, x2, y2, ms (i/u) and text (s) as `mirror-input` takes them.
    async fn mirror_input(
        &self,
        device_id: String,
        action: String,
        args: HashMap<String, zbus::zvariant::OwnedValue>,
    ) -> Result<(), LinkError> {
        let action: MirrorAction = serde_json::from_value(serde_json::Value::String(action))
            .map_err(|_| LinkError::Rejected("not a mirror action".to_owned()))?;
        let int = |key: &str| args.get(key).and_then(|value| i32::try_from(value.clone()).ok());
        let input = MirrorInput {
            action,
            x: int("x"),
            y: int("y"),
            x2: int("x2"),
            y2: int("y2"),
            ms: args.get("ms").and_then(|value| u32::try_from(value.clone()).ok()),
            text: args.get("text").and_then(|value| String::try_from(value.clone()).ok()),
        };
        self.send(&device_id, Message::MirrorInput(input)).await
    }

    /// Asks the phone for a keyframe, after the viewer lost one.
    async fn mirror_keyframe(&self, device_id: String) -> Result<(), LinkError> {
        self.send(&device_id, Message::MirrorKeyframe).await
    }

    /// Starts or stops ringing the phone.
    async fn ring(&self, device_id: String, on: bool) -> Result<(), LinkError> {
        self.send(&device_id, Message::Ring(Ring { on })).await
    }

    /// Reports this desktop's ringing, started or stopped by the shell, to the phone that asked for it.
    async fn desktop_ringing(&self, device_id: String, on: bool) -> Result<(), LinkError> {
        self.send(&device_id, Message::Ringing(Ringing { on })).await
    }

    /// `mute` silences the phone's ringer; `decline` ends its ringing call.
    async fn call_action(&self, device_id: String, action: String) -> Result<(), LinkError> {
        let action = CallActionKind::parse(&action)
            .ok_or_else(|| LinkError::Rejected("action is mute or decline".to_owned()))?;
        self.send(&device_id, Message::CallAction(CallAction { action })).await
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
    async fn call(
        emitter: &SignalEmitter<'_>,
        device_id: &str,
        state: &str,
        number: &str,
        name: &str,
    ) -> zbus::Result<()>;

    #[zbus(signal)]
    async fn ring_requested(emitter: &SignalEmitter<'_>, device_id: &str, on: bool) -> zbus::Result<()>;

    #[zbus(signal)]
    async fn phone_ringing(emitter: &SignalEmitter<'_>, device_id: &str, on: bool) -> zbus::Result<()>;

    /// The desktop joined the phone's hotspot `ssid` to move a transfer off Bluetooth; an empty `ssid` means it left.
    #[zbus(signal)]
    async fn hotspot(emitter: &SignalEmitter<'_>, device_id: &str, ssid: &str) -> zbus::Result<()>;

    #[zbus(signal)]
    async fn notification_removed(emitter: &SignalEmitter<'_>, device_id: &str, id: &str) -> zbus::Result<()>;
}

impl Link {
    /// Sends an unacknowledged message to a connected device.
    async fn browse(&self, device_id: &str, request: browse::Request) -> Result<browse::Answer, LinkError> {
        let id = DeviceId::parse(device_id).map_err(|_| LinkError::Rejected("not a device id".to_owned()))?;
        self.hub.browse(id, request).await.map_err(|failure| match failure {
            browse::Failure::Refused(reason) => LinkError::Refused(reason.as_str().to_owned()),
            browse::Failure::NotConnected => LinkError::NotConnected(format!("{device_id} is not connected")),
            browse::Failure::TimedOut => LinkError::Failed("the phone did not answer in time".to_owned()),
            browse::Failure::Malformed => LinkError::Failed("the phone's answer broke the protocol".to_owned()),
        })
    }

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

impl Link {
    async fn decide(&self, transfer_id: &str, accept: bool) -> Result<(), LinkError> {
        let id = parse_transfer(transfer_id)?;
        if self.hub.decide(id, accept).await {
            Ok(())
        } else {
            Err(LinkError::Rejected(format!("transfer {id} is not waiting for an answer")))
        }
    }
}

fn parse_transfer(text: &str) -> Result<TransferId, LinkError> {
    TransferId::parse_hex(text).ok_or_else(|| LinkError::Rejected("not a transfer id".to_owned()))
}

async fn forward_transfer(emitter: &SignalEmitter<'_>, signal: Signal) -> zbus::Result<()> {
    match signal {
        Signal::Offered { id, device, files } => Link::transfer_offered(emitter, &id.to_hex(), &device, files).await,
        Signal::Progress { id, bytes, total } => Link::transfer_progress(emitter, &id.to_hex(), bytes, total).await,
        Signal::Finished { id, status, paths } => {
            let paths = paths.iter().map(|path| path.to_string_lossy().into_owned()).collect();
            Link::transfer_finished(emitter, &id.to_hex(), status.as_str(), paths).await
        }
    }
}

pub async fn serve(
    bus: &zbus::Connection,
    hub: HubHandle,
    snapshots: watch::Receiver<Snapshot>,
    nearby: watch::Receiver<Vec<(String, String)>>,
) -> anyhow::Result<()> {
    bus.object_server().at(PATH, Link { hub, snapshots, nearby }).await?;
    bus.request_name(NAME).await?;
    Ok(())
}

/// Turns hub snapshots into `PropertiesChanged` and hub events into signals.
pub async fn forward(
    bus: &zbus::Connection,
    mut snapshots: watch::Receiver<Snapshot>,
    mut events: mpsc::Receiver<Event>,
    mut nearby: watch::Receiver<Vec<(String, String)>>,
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
                if next.auto_accept != last.auto_accept {
                    link.get().await.auto_accept_changed(emitter).await?;
                }
                if next.grants != last.grants {
                    link.get().await.grants_changed(emitter).await?;
                }
                if next.status != last.status {
                    link.get().await.device_status_changed(emitter).await?;
                }
                if next.localsend != last.localsend {
                    link.get().await.local_send_visible_changed(emitter).await?;
                }
                last = next;
            }
            changed = nearby.changed() => {
                changed?;
                link.get().await.nearby_changed(emitter).await?;
            }
            Some(event) = events.recv() => match event {
                Event::PairingFinished { id, name } => Link::pairing_finished(emitter, id.as_str(), &name).await?,
                Event::PairingFailed { reason } => Link::pairing_failed(emitter, &reason).await?,
                Event::Received { id, share } => {
                    Link::received(emitter, id.as_str(), share.kind.as_str(), &share.text).await?;
                }
                Event::Transfer(signal) => forward_transfer(emitter, signal).await?,
                Event::ClipboardOffered { from, id, mimes, size } => {
                    Link::clipboard_offered(emitter, from.as_str(), id, mimes, size).await?;
                }
                Event::NotificationPosted { id, posted } => emit_posted(emitter, &id, posted).await?,
                Event::RingRequested { id, on } => Link::ring_requested(emitter, id.as_str(), on).await?,
                Event::PhoneRinging { id, on } => Link::phone_ringing(emitter, id.as_str(), on).await?,
                Event::Hotspot { id, ssid } => Link::hotspot(emitter, id.as_str(), &ssid.unwrap_or_default()).await?,
                Event::Call { id, call } => {
                    let (number, name) = (call.number.unwrap_or_default(), call.name.unwrap_or_default());
                    Link::call(emitter, id.as_str(), call.state.as_str(), &number, &name).await?;
                }
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
