//! `org.umbriel.Link1` on the session bus; contract in `protocol/link-v1/org.umbriel.Link1.xml`.

use std::collections::HashMap;
use std::fs::File;
use std::io::Read as _;
use std::os::fd::OwnedFd;

use link_core::Error;
use link_core::identity::DeviceId;
use link_core::proto::message::{MAX_NAME_LEN, MAX_SHARE_LEN, Share, ShareKind, TransferId, is_text_mime};
use link_core::transfer::{LocalClip, Source, TransferEvent};
use tokio::sync::{mpsc, watch};
use zbus::fdo;
use zbus::object_server::SignalEmitter;

use crate::hub::{Event, HubHandle, Snapshot};

const PATH: &str = "/org/umbriel/Link1";
const NAME: &str = "org.umbriel.Link1";
/// The shell passes descriptors, not types; the receiver's platform types files by name.
const OCTET_STREAM: &str = "application/octet-stream";

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

    /// Offers the files behind the descriptors to a connected device and returns the transfer id at once; the
    /// transfer signals report the rest.
    async fn send_files(
        &self,
        device_id: String,
        files: Vec<(zbus::zvariant::OwnedFd, String)>,
    ) -> Result<String, LinkError> {
        let id = DeviceId::parse(&device_id).map_err(|_| LinkError::Rejected("not a device id".to_owned()))?;
        if files.is_empty() {
            return Err(LinkError::Rejected("no files".to_owned()));
        }
        let sources = files
            .into_iter()
            .map(|(fd, name)| Source::new(File::from(OwnedFd::from(fd)), name, OCTET_STREAM.to_owned()))
            .collect::<Result<Vec<_>, _>>()
            .map_err(|error| LinkError::Rejected(error.to_string()))?;
        match self.hub.transfers().send(id.clone(), sources).await {
            Ok(transfer) => Ok(transfer.to_hex()),
            Err(Error::NotConnected) => Err(LinkError::NotConnected(format!("{id} is not connected"))),
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
        match self.hub.transfers().cancel(id).await {
            Ok(true) => Ok(()),
            Ok(false) => Err(LinkError::Rejected(format!("no open transfer {id}"))),
            Err(error) => Err(LinkError::Failed(error.to_string())),
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
}

impl Link {
    async fn decide(&self, transfer_id: &str, accept: bool) -> Result<(), LinkError> {
        let id = parse_transfer(transfer_id)?;
        match self.hub.transfers().decide(id, accept).await {
            Ok(true) => Ok(()),
            Ok(false) => Err(LinkError::Rejected(format!("transfer {id} is not waiting for an answer"))),
            Err(error) => Err(LinkError::Failed(error.to_string())),
        }
    }
}

fn parse_transfer(text: &str) -> Result<TransferId, LinkError> {
    TransferId::parse_hex(text).ok_or_else(|| LinkError::Rejected("not a transfer id".to_owned()))
}

async fn forward_transfer(emitter: &SignalEmitter<'_>, event: TransferEvent) -> zbus::Result<()> {
    match event {
        TransferEvent::Offered { id, from, files } => {
            let files = files.into_iter().map(|file| (file.name, file.size)).collect();
            Link::transfer_offered(emitter, &id.to_hex(), from.as_str(), files).await
        }
        TransferEvent::Progress { id, bytes, total } => {
            Link::transfer_progress(emitter, &id.to_hex(), bytes, total).await
        }
        TransferEvent::Finished { id, status, files, .. } => {
            let paths = files.iter().map(|file| file.path.to_string_lossy().into_owned()).collect();
            Link::transfer_finished(emitter, &id.to_hex(), status.as_str(), paths).await
        }
        TransferEvent::ClipOffered { from, id, mimes, size, .. } => {
            Link::clipboard_offered(emitter, from.as_str(), id, mimes, size).await
        }
        TransferEvent::Busy { .. } => Ok(()),
    }
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
                if next.auto_accept != last.auto_accept {
                    link.get().await.auto_accept_changed(emitter).await?;
                }
                if next.grants != last.grants {
                    link.get().await.grants_changed(emitter).await?;
                }
                last = next;
            }
            Some(event) = events.recv() => match event {
                Event::PairingFinished { id, name } => Link::pairing_finished(emitter, id.as_str(), &name).await?,
                Event::PairingFailed { reason } => Link::pairing_failed(emitter, &reason).await?,
                Event::Received { id, share } => {
                    Link::received(emitter, id.as_str(), share.kind.as_str(), &share.text).await?;
                }
                Event::Transfer(event) => forward_transfer(emitter, event).await?,
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
