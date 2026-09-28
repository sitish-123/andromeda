//! The hub actor: sole owner of the device store, the pairing window, and the set of live sessions.

use std::collections::{HashMap, HashSet};
use std::path::PathBuf;
use std::sync::Arc;
use std::time::Duration;

use link_core::discovery::Advertiser;
use link_core::identity::{DeviceId, Spki};
use link_core::net;
use link_core::proto::CloseCode;
use link_core::proto::message::{
    Call, CallState, Hotspot, HotspotEnd, HotspotJoined, MediaPlayer, Message, NotificationPosted, Share, Status,
    TransferId,
};
use link_core::proto::pairing::{Secret, Secrets};
use link_core::session::{Route, SessionEvent, SessionHandle};
use link_core::store::{Peer, Store, feature_of};
use link_core::transfer::{Source, Status as TransferStatus, TransferEvent, TransferHandle};
use link_core::transport::Puncher;
use link_core::uri::{PairingUri, QR_SECRET_LEN};
use ring::rand::{SecureRandom, SystemRandom};
use tokio::sync::{mpsc, oneshot, watch};
use tokio::task::JoinSet;
use tokio::time::Instant;
use zbus::zvariant::OwnedObjectPath;

use crate::browse;
use crate::desktop_media::{DesktopMediaHandle, Request};
use crate::hotspot;
use crate::localsend::LocalSendHandle;
use crate::media::Media;
use crate::notifications::{Change, Mirror};
use crate::paths::Paths;

const WINDOW: Duration = Duration::from_secs(120);

pub enum Admission {
    Session,
    Pair(Attempt),
    Reject(CloseCode),
}

/// One pairing attempt: the secrets of the window that admitted it, and that window's id.
pub struct Attempt {
    pub secrets: Arc<Secrets>,
    pub window: u64,
}

#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct Snapshot {
    /// `(device_id, name, connected)`, the D-Bus `Devices` property.
    pub devices: Vec<(String, String, bool)>,
    pub pairing: bool,
    /// Devices whose file offers are accepted without asking, the D-Bus `AutoAccept` property.
    pub auto_accept: Vec<String>,
    /// Device id to granted features, the D-Bus `Grants` property.
    pub grants: Vec<(String, Vec<String>)>,
    /// Connected device id to `(battery, charging, network)`, the D-Bus `DeviceStatus` property.
    pub status: Vec<(String, (u32, bool, String))>,
    /// "Visible to LocalSend", the D-Bus `LocalSendVisible` property.
    pub localsend: bool,
}

#[derive(Debug)]
pub enum Event {
    PairingFinished {
        id: DeviceId,
        name: String,
    },
    PairingFailed {
        reason: String,
    },
    Received {
        id: DeviceId,
        share: Share,
    },
    /// A transfer in either direction, from any backend.
    Transfer(Signal),
    ClipboardOffered {
        from: DeviceId,
        id: u64,
        mimes: Vec<String>,
        size: u64,
    },
    NotificationPosted {
        id: DeviceId,
        posted: NotificationPosted,
    },
    NotificationRemoved {
        id: DeviceId,
        notification: String,
    },
    /// The phone asks this desktop to ring, or to stop.
    RingRequested {
        id: DeviceId,
        on: bool,
    },
    PhoneRinging {
        id: DeviceId,
        on: bool,
    },
    Call {
        id: DeviceId,
        call: Call,
    },
    /// The desktop joined a phone's hotspot (`ssid`), or left it (None).
    Hotspot {
        id: DeviceId,
        ssid: Option<String>,
    },
}

/// What any transfer backend reports, as the shell sees it: the Link transfer actor and LocalSend alike.
#[derive(Debug, Clone)]
pub enum Signal {
    /// An offer waiting for consent; `device` is a Link device id or `localsend:<fingerprint>`.
    Offered {
        id: TransferId,
        device: String,
        files: Vec<(String, u64)>,
    },
    Progress {
        id: TransferId,
        bytes: u64,
        total: u64,
    },
    Finished {
        id: TransferId,
        status: TransferStatus,
        paths: Vec<PathBuf>,
    },
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum Backend {
    Link,
    LocalSend,
}

enum Command {
    StartPairing { reply: oneshot::Sender<anyhow::Result<(String, String)>> },
    CancelPairing,
    Unpair { id: DeviceId, reply: oneshot::Sender<bool> },
    Admit { spki: Spki, reply: oneshot::Sender<Admission> },
    Paired { spki: Spki, name: String, window: u64 },
    PairingFailed { reason: String, window: u64 },
    Connected { id: DeviceId, name: String, session: SessionHandle },
    Disconnected { id: DeviceId, stable_id: usize },
    Session { id: DeviceId, reply: oneshot::Sender<Option<SessionHandle>> },
    SetAutoAccept { id: DeviceId, enabled: bool, reply: oneshot::Sender<bool> },
    SetGrant { id: DeviceId, feature: String, granted: bool, reply: oneshot::Sender<bool> },
    SendFiles { device: String, sources: Vec<Source>, reply: oneshot::Sender<Result<TransferId, link_core::Error>> },
    Decide { id: TransferId, accept: bool, reply: oneshot::Sender<bool> },
    CancelTransfer { id: TransferId, reply: oneshot::Sender<bool> },
    SetLocalSend { visible: bool },
    ClipboardPeers { reply: oneshot::Sender<Vec<DeviceId>> },
    DesktopPlayer { player: MediaPlayer },
    DesktopPlayerGone { player: String },
    Browse { id: DeviceId, request: browse::Request, reply: browse::Reply },
}

#[derive(Clone)]
pub struct HubHandle {
    commands: mpsc::Sender<Command>,
    session_events: mpsc::Sender<SessionEvent>,
    transfers: TransferHandle,
}

struct Window {
    id: u64,
    secrets: Arc<Secrets>,
    deadline: Instant,
    taken: bool,
}

pub struct Hub {
    own: Spki,
    store: Store,
    paths: Paths,
    /// Present only while someone could be looking: a paired device or an open pairing window.
    advertiser: Option<Advertiser>,
    window: Option<Window>,
    windows_opened: u64,
    sessions: HashMap<DeviceId, SessionHandle>,
    status: HashMap<DeviceId, Status>,
    notifications: Mirror,
    media: Media,
    /// Phones with a call ringing or active, during which the desktop's players stay paused.
    in_call: HashSet<DeviceId>,
    /// Its own handle, for the exported MPRIS players to reach sessions.
    me: HubHandle,
    commands: mpsc::Receiver<Command>,
    session_events: mpsc::Receiver<SessionEvent>,
    snapshots: watch::Sender<Snapshot>,
    events: mpsc::Sender<Event>,
    transfers: TransferHandle,
    transfer_events: mpsc::UnboundedReceiver<TransferEvent>,
    localsend: LocalSendHandle,
    localsend_signals: mpsc::UnboundedReceiver<Signal>,
    /// Which backend each open transfer belongs to, for consent and cancelling.
    open: HashMap<TransferId, Backend>,
    /// Phones whose hotspot this desktop joined, and the `NetworkManager` connection it joined with.
    hotspots: HashMap<DeviceId, OwnedObjectPath>,
    joining: JoinSet<(DeviceId, String, Result<hotspot::Joined, String>)>,
    /// Sends too large for Bluetooth, waiting for the phone's session to move to its hotspot.
    waiting: Vec<Waiting>,
    browsing: browse::Browsing,
    puncher: Puncher,
}

struct Waiting {
    peer: DeviceId,
    sources: Vec<Source>,
    reply: oneshot::Sender<Result<TransferId, link_core::Error>>,
    deadline: Instant,
}

/// The transfer backends the hub consents for: the Link transfer actor, which it also attaches sessions to, and
/// LocalSend.
pub struct Transfers {
    pub handle: TransferHandle,
    pub events: mpsc::UnboundedReceiver<TransferEvent>,
    pub localsend: LocalSendHandle,
    pub localsend_signals: mpsc::UnboundedReceiver<Signal>,
}

impl Hub {
    pub fn new(
        own: Spki,
        store: Store,
        paths: Paths,
        desktop_media: DesktopMediaHandle,
        transfers: Transfers,
        puncher: Puncher,
    ) -> (Self, HubHandle, watch::Receiver<Snapshot>, mpsc::Receiver<Event>) {
        let (commands_tx, commands) = mpsc::channel(32);
        let (session_events_tx, session_events) = mpsc::channel(16);
        let handle =
            HubHandle { commands: commands_tx, session_events: session_events_tx, transfers: transfers.handle.clone() };
        let media = Media::new(desktop_media, paths.art.clone());
        let (snapshots, snapshots_rx) = watch::channel(Snapshot::default());
        let (events, events_rx) = mpsc::channel(16);
        let mut hub = Self {
            own,
            store,
            paths,
            advertiser: None,
            window: None,
            windows_opened: 0,
            sessions: HashMap::new(),
            status: HashMap::new(),
            notifications: Mirror::default(),
            media,
            in_call: HashSet::new(),
            me: handle.clone(),
            commands,
            session_events,
            snapshots,
            events,
            transfers: transfers.handle.clone(),
            transfer_events: transfers.events,
            localsend: transfers.localsend,
            localsend_signals: transfers.localsend_signals,
            open: HashMap::new(),
            hotspots: HashMap::new(),
            joining: JoinSet::new(),
            waiting: Vec::new(),
            browsing: browse::Browsing::default(),
            puncher,
        };
        hub.refresh();
        (hub, handle, snapshots_rx, events_rx)
    }

    pub async fn run(mut self) -> anyhow::Result<()> {
        if self.store.localsend {
            self.localsend.set_visible(true).await;
        }
        loop {
            let deadline = self.window.as_ref().map(|window| window.deadline);
            let give_up = self.waiting.iter().map(|waiting| waiting.deadline).min();
            let browse_deadline = self.browsing.deadline();
            tokio::select! {
                () = sleep_until(browse_deadline) => self.browsing.expire(),
                Some(joined) = self.joining.join_next() => match joined {
                    Ok((id, ssid, joined)) => self.hotspot_joined(id, ssid, joined).await,
                    Err(join) => log::warn!("hotspot join task: {join}"),
                },
                () = sleep_until(give_up) => self.expire_waiting().await,
                command = self.commands.recv() => match command {
                    Some(command) => self.handle(command).await,
                    None => return Ok(()),
                },
                Some(event) = self.session_events.recv() => self.on_session_event(event).await,
                Some(event) = self.transfer_events.recv() => self.on_transfer(event).await,
                Some(signal) = self.localsend_signals.recv() => self.on_signal(Backend::LocalSend, signal).await,
                () = sleep_until(deadline) => {
                    log::info!("pairing window expired");
                    self.close_window();
                }
            }
        }
    }

    async fn handle(&mut self, command: Command) {
        match command {
            Command::StartPairing { reply } => drop(reply.send(self.start_pairing())),
            Command::CancelPairing => self.close_window(),
            Command::Unpair { id, reply } => {
                let unpaired = self.unpair(&id);
                self.forget_notifications(&id).await;
                self.media.disconnected(&id).await;
                let _ = reply.send(unpaired);
            }
            Command::Admit { spki, reply } => drop(reply.send(self.admit(&spki))),
            Command::Paired { spki, name, window } => self.paired(&spki, name, window).await,
            Command::PairingFailed { reason, window } => {
                if self.close_window_if(window) {
                    self.emit(Event::PairingFailed { reason }).await;
                }
            }
            Command::Connected { id, name, session } => self.connected(id, name, session).await,
            Command::Disconnected { id, stable_id } => {
                self.transfers.detach(id.clone(), stable_id).await;
                if self.sessions.get(&id).is_some_and(|live| live.stable_id() == stable_id) {
                    self.sessions.remove(&id);
                    self.browsing.disconnected(&id);
                    self.leave_hotspot(&id).await;
                    self.status.remove(&id);
                    self.publish();
                    self.media.disconnected(&id).await;
                    self.set_in_call(&id, false).await;
                }
            }
            Command::Browse { id, request, reply } => {
                let Some(session) = self.sessions.get(&id).filter(|session| session.is_live()).cloned() else {
                    return drop(reply.send(Err(browse::Failure::NotConnected)));
                };
                let (req, message) = self.browsing.start(id, request, reply);
                if session.send(message).await.is_err() {
                    self.browsing.fail(req, browse::Failure::NotConnected);
                }
            }
            Command::DesktopPlayer { player } => self.media.desktop_player(player, self.sessions.values()),
            Command::DesktopPlayerGone { player } => self.media.desktop_gone(player, self.sessions.values()),
            Command::Session { id, reply } => {
                drop(reply.send(self.sessions.get(&id).filter(|session| session.is_live()).cloned()));
            }
            Command::SetAutoAccept { id, enabled, reply } => {
                let Some(peer) = self.store.peer_mut(&id) else {
                    let _ = reply.send(false);
                    return;
                };
                peer.auto_accept = enabled;
                self.save();
                self.publish();
                let _ = reply.send(true);
            }
            Command::SetGrant { id, feature, granted, reply } => {
                let feature = link_core::store::Feature::parse(&feature);
                let set = match (self.store.peer_mut(&id), feature) {
                    (Some(peer), Some(feature)) => {
                        peer.grants.set(feature, granted);
                        true
                    }
                    _ => false,
                };
                if set {
                    self.save();
                    self.publish();
                }
                let _ = reply.send(set);
            }
            Command::SendFiles { device, sources, reply } => match self.needs_hotspot(&device, &sources) {
                Some(peer) => self.wait_for_hotspot(peer, sources, reply).await,
                None => drop(reply.send(self.send_files(&device, sources).await)),
            },
            Command::Decide { id, accept, reply } => {
                let answered = match self.open.get(&id) {
                    Some(Backend::LocalSend) => self.localsend.decide(id, accept).await,
                    _ => self.transfers.decide(id, accept).await.unwrap_or(false),
                };
                let _ = reply.send(answered);
            }
            Command::CancelTransfer { id, reply } => {
                let cancelled = match self.open.get(&id) {
                    Some(Backend::LocalSend) => self.localsend.cancel(id).await,
                    _ => self.transfers.cancel(id).await.unwrap_or(false),
                };
                let _ = reply.send(cancelled);
            }
            Command::SetLocalSend { visible } => {
                self.store.localsend = visible;
                self.save();
                self.publish();
                self.localsend.set_visible(visible).await;
            }
            Command::ClipboardPeers { reply } => {
                let granted = |id: &DeviceId| self.store.peer(id).is_some_and(|peer| peer.grants.clipboard);
                let peers = self.sessions.iter().filter(|(id, session)| session.is_live() && granted(id));
                drop(reply.send(peers.map(|(id, _)| id.clone()).collect()));
            }
        }
    }

    /// Consent and grants are the hub's: an offer from a device without the files grant is declined, one from an
    /// auto-accept device accepted, any other goes to the shell; a clip from a device without the clipboard grant is
    /// dropped.
    async fn on_transfer(&mut self, event: TransferEvent) {
        let grants = |id: &DeviceId| self.store.peer(id).map(|peer| peer.grants).unwrap_or_default();
        match &event {
            TransferEvent::Busy { .. } => return,
            TransferEvent::Offered { id, from, .. } if !grants(from).files => {
                log::info!("{from}: declining transfer {id}: no files grant");
                if let Err(error) = self.transfers.decide(*id, false).await {
                    log::error!("declining {id}: {error}");
                }
                return;
            }
            TransferEvent::ClipOffered { from, .. } if !grants(from).clipboard => {
                log::info!("{from}: dropping a clipboard offer: no clipboard grant");
                return;
            }
            TransferEvent::Offered { id, from, .. } if self.store.peer(from).is_some_and(|peer| peer.auto_accept) => {
                log::info!("{from}: auto-accepting transfer {id}");
                if let Err(error) = self.transfers.decide(*id, true).await {
                    log::error!("accepting {id}: {error}");
                }
                return;
            }
            _ => {}
        }
        let signal = match event {
            TransferEvent::Offered { id, from, files } => {
                let files = files.into_iter().map(|file| (file.name, file.size)).collect();
                Signal::Offered { id, device: from.to_string(), files }
            }
            TransferEvent::Progress { id, bytes, total } => Signal::Progress { id, bytes, total },
            TransferEvent::Finished { id, status, files, .. } => {
                Signal::Finished { id, status, paths: files.into_iter().map(|file| file.path).collect() }
            }
            TransferEvent::ClipOffered { from, id, mimes, size, .. } => {
                return self.emit(Event::ClipboardOffered { from, id, mimes, size }).await;
            }
            TransferEvent::Busy { .. } => return,
        };
        self.on_signal(Backend::Link, signal).await;
    }

    /// Tracks which backend owns each open transfer and passes the signal to D-Bus.
    async fn on_signal(&mut self, backend: Backend, signal: Signal) {
        match &signal {
            Signal::Offered { id, .. } | Signal::Progress { id, .. } => {
                self.open.entry(*id).or_insert(backend);
            }
            Signal::Finished { id, .. } => {
                self.open.remove(id);
            }
        }
        self.emit(Event::Transfer(signal)).await;
    }

    /// A `localsend:<fingerprint>` device goes to LocalSend, anything else to Link.
    async fn send_files(&mut self, device: &str, sources: Vec<Source>) -> Result<TransferId, link_core::Error> {
        if let Some(fingerprint) = device.strip_prefix(crate::localsend::PREFIX) {
            let id = self
                .localsend
                .send(fingerprint.to_owned(), sources)
                .await
                .map_err(|_| link_core::Error::NotConnected)?;
            self.open.insert(id, Backend::LocalSend);
            return Ok(id);
        }
        let id = self.transfers.send(DeviceId::parse(device)?, sources).await?;
        self.open.insert(id, Backend::Link);
        Ok(id)
    }

    async fn on_session_event(&mut self, event: SessionEvent) {
        match event {
            SessionEvent::Received { from, share } => {
                if self.sessions.contains_key(&from) {
                    self.emit(Event::Received { id: from, share }).await;
                }
            }
            SessionEvent::Status { from, status } => {
                if self.sessions.contains_key(&from) {
                    self.status.insert(from, status);
                    self.publish();
                }
            }
            SessionEvent::Unpaired { from } => {
                log::info!("{from} unpaired itself");
                self.sessions.remove(&from);
                self.store.remove(&from);
                self.save();
                self.refresh();
                self.forget_notifications(&from).await;
                self.media.disconnected(&from).await;
            }
            SessionEvent::Message {
                from,
                message: answer @ (Message::FsEntries(_) | Message::FsData(_) | Message::FsError(_)),
            } => {
                // Answers to this desktop's own requests, which need no grant of the phone's here.
                if let Some(next) = self.browsing.on_answer(&from, answer)
                    && let Some(session) = self.sessions.get(&from)
                    && session.send(next).await.is_err()
                {
                    log::info!("{from}: asking for the next page of a listing failed");
                }
            }
            SessionEvent::Message { from, message: Message::Punch(punch) } if self.sessions.contains_key(&from) => {
                let addresses: Vec<std::net::SocketAddr> =
                    punch.addresses.iter().filter_map(|text| text.parse().ok()).collect();
                log::info!("{from}: punching toward {addresses:?}");
                self.puncher.punch(&addresses);
            }
            SessionEvent::Message { from, message } => {
                let granted = feature_of(&message)
                    .is_some_and(|feature| self.store.peer(&from).is_some_and(|peer| peer.grants.allows(feature)));
                if !granted {
                    log::info!("{from}: dropped a {} its grant does not allow", message.kind());
                } else if self.sessions.contains_key(&from) {
                    self.on_message(from, message).await;
                }
            }
        }
    }

    async fn on_message(&mut self, from: DeviceId, message: Message) {
        match message {
            Message::NotificationPosted(posted) => {
                for change in self.notifications.post(&from, posted) {
                    let event = match change {
                        Change::Posted(posted) => Event::NotificationPosted { id: from.clone(), posted },
                        Change::Removed(notification) => Event::NotificationRemoved { id: from.clone(), notification },
                    };
                    self.emit(event).await;
                }
            }
            Message::NotificationRemoved(removed) => {
                if self.notifications.remove(&from, &removed.id) {
                    self.emit(Event::NotificationRemoved { id: from, notification: removed.id }).await;
                }
            }
            media @ (Message::MediaPlayer(_) | Message::MediaGone(_) | Message::MediaCommand(_)) => {
                let name = self.store.peer(&from).map(|peer| peer.name.clone()).unwrap_or_default();
                self.media.on_phone_message(&self.me, &from, &name, media).await;
            }
            Message::Ring(ring) => self.emit(Event::RingRequested { id: from, on: ring.on }).await,
            Message::Call(call) => {
                self.set_in_call(&from, call.state != CallState::Idle).await;
                self.emit(Event::Call { id: from, call }).await;
            }
            Message::Ringing(ringing) => self.emit(Event::PhoneRinging { id: from, on: ringing.on }).await,
            Message::Hotspot(offered) => self.join_hotspot(from, offered),
            Message::HotspotEnd(end) => {
                log::info!("{from}: the phone ended its hotspot: {:?}", end.reason);
                let reason = end.reason.unwrap_or_else(|| "the phone ended its hotspot".to_owned());
                self.fail_waiting(&from, &reason).await;
                self.leave_hotspot(&from).await;
            }
            other => log::warn!("{from}: a desktop session delivered {}", other.kind()),
        }
    }

    /// Pauses the desktop's players when a first phone's call starts, and resumes them when the last one ends.
    async fn set_in_call(&mut self, id: &DeviceId, calling: bool) {
        let before = self.in_call.is_empty();
        if calling {
            self.in_call.insert(id.clone());
        } else {
            self.in_call.remove(id);
        }
        match (before, self.in_call.is_empty()) {
            (true, false) => self.media.request(Request::PauseAll).await,
            (false, true) => self.media.request(Request::ResumePaused).await,
            _ => {}
        }
    }

    async fn forget_notifications(&mut self, id: &DeviceId) {
        for notification in self.notifications.forget(id) {
            self.emit(Event::NotificationRemoved { id: id.clone(), notification }).await;
        }
    }

    fn start_pairing(&mut self) -> anyhow::Result<(String, String)> {
        let (code, qr) = generate_secrets()?;
        let uri = PairingUri {
            fingerprint: self.own.fingerprint(),
            secret: qr,
            addresses: net::local_addresses(self.store.port),
        };
        let secrets = Secrets { code: Secret::new(code.clone().into_bytes()), qr: Secret::new(qr.to_vec()) };
        self.windows_opened += 1;
        self.window = Some(Window {
            id: self.windows_opened,
            secrets: Arc::new(secrets),
            deadline: Instant::now() + WINDOW,
            taken: false,
        });
        self.refresh();
        log::info!("pairing window open");
        Ok((code, uri.to_string()))
    }

    fn close_window(&mut self) {
        if self.window.take().is_some() {
            self.refresh();
        }
    }

    /// Closes the window only if it is still `id`, so an attempt on a replaced window leaves the new one open.
    fn close_window_if(&mut self, id: u64) -> bool {
        if self.window.as_ref().is_none_or(|window| window.id != id) {
            return false;
        }
        self.close_window();
        true
    }

    fn admit(&mut self, spki: &Spki) -> Admission {
        let id = spki.device_id();
        if let Some(index) = self.store.revoked.iter().position(|revoked| *revoked == id) {
            self.store.revoked.remove(index);
            self.save();
            return Admission::Reject(CloseCode::Unpaired);
        }
        if self.store.peer(&id).is_some() {
            return Admission::Session;
        }
        match &mut self.window {
            Some(window) if window.taken => Admission::Reject(CloseCode::Busy),
            Some(window) => {
                log::info!("{id}: pairing attempt on window {}", window.id);
                window.taken = true;
                Admission::Pair(Attempt { secrets: window.secrets.clone(), window: window.id })
            }
            None => Admission::Reject(CloseCode::NotPaired),
        }
    }

    async fn paired(&mut self, spki: &Spki, name: String, window: u64) {
        let mut peer = Peer::new(spki, name.clone());
        peer.touch();
        let id = peer.id.clone();
        self.store.upsert(peer);
        self.save();
        log::info!("paired {id} ({name:?})");
        if self.close_window_if(window) {
            self.emit(Event::PairingFinished { id, name }).await;
        } else {
            self.refresh();
        }
    }

    /// One session per device: a newer one replaces the older, which a phone that changed networks leaves behind.
    async fn connected(&mut self, id: DeviceId, name: String, session: SessionHandle) {
        let Some(peer) = self.store.peer_mut(&id) else {
            return session.close(CloseCode::NotPaired);
        };
        peer.name = name;
        peer.touch();
        self.save();
        self.media.connected(&session);
        let on_ip = session.connection().quic().is_some();
        if let Some(older) = self.sessions.insert(id.clone(), session.clone()) {
            older.close(CloseCode::Done);
        }
        self.publish();
        self.transfers.attach(id.clone(), session).await;
        if on_ip {
            self.send_waiting(&id).await;
        } else {
            // Back on Bluetooth: the phone's hotspot is gone even if its hotspot-end was lost with the old session.
            self.leave_hotspot(&id).await;
        }
    }

    /// The Link device a send is for, when its session runs over Bluetooth and the files are worth the hotspot.
    fn needs_hotspot(&self, device: &str, sources: &[Source]) -> Option<DeviceId> {
        let peer = DeviceId::parse(device).ok()?;
        let on_bluetooth = self.sessions.get(&peer)?.connection().quic().is_none();
        (on_bluetooth && total_size(sources) > link_core::stream::BLUETOOTH_UPGRADE_ABOVE).then_some(peer)
    }

    /// Asks the phone for its hotspot and parks the send until the session moves there.
    async fn wait_for_hotspot(
        &mut self,
        peer: DeviceId,
        sources: Vec<Source>,
        reply: oneshot::Sender<Result<TransferId, link_core::Error>>,
    ) {
        let asked = match self.sessions.get(&peer) {
            Some(session) => session.send(Message::HotspotRequest).await,
            None => Err(link_core::Error::NotConnected),
        };
        match asked {
            Ok(()) => {
                let deadline = Instant::now() + link_core::hotspot::UPGRADE_TIMEOUT;
                self.waiting.push(Waiting { peer, sources, reply, deadline });
            }
            Err(error) => drop(reply.send(Err(error))),
        }
    }

    async fn send_waiting(&mut self, id: &DeviceId) {
        let (ready, rest) = std::mem::take(&mut self.waiting).into_iter().partition(|waiting| waiting.peer == *id);
        self.waiting = rest;
        for Waiting { peer, sources, reply, .. } in ready {
            let sent = self.send_files(peer.as_str(), sources).await;
            drop(reply.send(sent));
        }
    }

    async fn fail_waiting(&mut self, id: &DeviceId, reason: &str) {
        let (failed, rest) = std::mem::take(&mut self.waiting).into_iter().partition(|waiting| waiting.peer == *id);
        self.waiting = rest;
        for waiting in failed {
            self.send_without_hotspot(waiting, link_core::Error::Hotspot(reason.to_owned())).await;
        }
    }

    async fn expire_waiting(&mut self) {
        let now = Instant::now();
        let (expired, rest) =
            std::mem::take(&mut self.waiting).into_iter().partition(|waiting| waiting.deadline <= now);
        self.waiting = rest;
        for waiting in expired {
            log::info!("{}: the session did not move to the phone's hotspot in time", waiting.peer);
            self.send_without_hotspot(waiting, link_core::Error::TooLargeForBluetooth).await;
        }
    }

    /// Sends over Bluetooth what fits there, and fails the rest with `error`.
    async fn send_without_hotspot(&mut self, waiting: Waiting, error: link_core::Error) {
        let Waiting { peer, sources, reply, .. } = waiting;
        if total_size(&sources) > link_core::stream::BLUETOOTH_FILE_LIMIT {
            return drop(reply.send(Err(error)));
        }
        log::info!("{peer}: sending over Bluetooth: {error}");
        let sent = self.send_files(peer.as_str(), sources).await;
        drop(reply.send(sent));
    }

    /// Joins the phone's hotspot off the actor; only a Bluetooth session needs one.
    fn join_hotspot(&mut self, id: DeviceId, offered: Hotspot) {
        let on_bluetooth = self.sessions.get(&id).is_some_and(|session| session.connection().quic().is_none());
        if !on_bluetooth || self.hotspots.contains_key(&id) {
            return log::info!("{id}: ignoring a hotspot this session does not need");
        }
        log::info!("{id}: joining the phone's hotspot {:?}", offered.ssid);
        self.joining.spawn(async move {
            let joined = hotspot::join(&offered.ssid, &offered.passphrase).await;
            (id, offered.ssid, joined)
        });
    }

    async fn hotspot_joined(&mut self, id: DeviceId, ssid: String, joined: Result<hotspot::Joined, String>) {
        let Some(session) = self.sessions.get(&id).cloned() else {
            if let Ok(joined) = joined {
                hotspot::leave(&joined.active).await;
            }
            return;
        };
        let reply = match joined {
            Ok(joined) => {
                let address = std::net::SocketAddr::new(joined.address, self.store.port).to_string();
                log::info!("{id}: joined the hotspot at {address}");
                self.hotspots.insert(id.clone(), joined.active);
                self.emit(Event::Hotspot { id: id.clone(), ssid: Some(ssid) }).await;
                Message::HotspotJoined(HotspotJoined { address })
            }
            Err(reason) => {
                log::info!("{id}: joining the hotspot failed: {reason}");
                self.fail_waiting(&id, &reason).await;
                let reason = reason.chars().take(link_core::proto::message::MAX_REASON_LEN).collect();
                Message::HotspotEnd(HotspotEnd { reason: Some(reason) })
            }
        };
        if let Err(error) = session.send(reply).await {
            log::info!("{id}: answering the hotspot: {error}");
        }
    }

    async fn leave_hotspot(&mut self, id: &DeviceId) {
        if let Some(active) = self.hotspots.remove(id) {
            hotspot::leave(&active).await;
            self.emit(Event::Hotspot { id: id.clone(), ssid: None }).await;
        }
    }

    fn unpair(&mut self, id: &DeviceId) -> bool {
        if self.store.remove(id).is_none() {
            return false;
        }
        if let Some(session) = self.sessions.remove(id) {
            session.close(CloseCode::Unpaired);
        }
        self.store.revoked.push(id.clone());
        self.save();
        self.refresh();
        true
    }

    /// Publishes the D-Bus snapshot and starts, updates, or stops the mDNS advertisement to match.
    fn refresh(&mut self) {
        self.publish();
        let pairing = self.window.is_some();
        if !pairing && self.store.peers.is_empty() {
            self.advertiser = None;
            return;
        }
        if let Some(advertiser) = &self.advertiser {
            return log_mdns(advertiser.set_pairing(pairing));
        }
        match Advertiser::start(&self.own.device_id(), self.store.port) {
            Ok(advertiser) => {
                log_mdns(advertiser.set_pairing(pairing));
                self.advertiser = Some(advertiser);
            }
            Err(error) => log::warn!("mdns: {error}"),
        }
    }

    fn save(&self) {
        if let Err(error) = self.store.save(&self.paths.devices) {
            log::error!("saving the device store: {error}");
        }
    }

    fn publish(&self) {
        let devices = self
            .store
            .peers
            .iter()
            .map(|peer| (peer.id.to_string(), peer.name.clone(), self.sessions.contains_key(&peer.id)))
            .collect();
        let auto_accept =
            self.store.peers.iter().filter(|peer| peer.auto_accept).map(|peer| peer.id.to_string()).collect();
        let grants = self.store.peers.iter().map(|peer| (peer.id.to_string(), peer.grants.names())).collect();
        let mut status: Vec<_> = self
            .status
            .iter()
            .map(|(id, status)| {
                let value = (u32::from(status.battery), status.charging, status.network.as_str().to_owned());
                (id.to_string(), value)
            })
            .collect();
        status.sort();
        self.snapshots.send_if_modified(|current| {
            let localsend = self.store.localsend;
            let next = Snapshot { devices, pairing: self.window.is_some(), auto_accept, grants, status, localsend };
            let changed = *current != next;
            *current = next;
            changed
        });
    }

    async fn emit(&self, event: Event) {
        if self.events.send(event).await.is_err() {
            log::debug!("no D-Bus forwarder for hub events");
        }
    }
}

fn log_mdns(result: Result<(), link_core::Error>) {
    if let Err(error) = result {
        log::warn!("mdns: {error}");
    }
}

fn total_size(sources: &[Source]) -> u64 {
    sources.iter().map(Source::size).sum()
}

async fn sleep_until(deadline: Option<Instant>) {
    match deadline {
        Some(deadline) => tokio::time::sleep_until(deadline).await,
        None => std::future::pending().await,
    }
}

/// A uniform 6-digit code and a 128-bit QR secret.
fn generate_secrets() -> anyhow::Result<(String, [u8; QR_SECRET_LEN])> {
    const CODES: u32 = 1_000_000;
    let rng = SystemRandom::new();
    let fill = |bytes: &mut [u8]| rng.fill(bytes).map_err(|_| anyhow::anyhow!("system randomness unavailable"));
    let code = loop {
        let mut bytes = [0; 4];
        fill(&mut bytes)?;
        let value = u32::from_be_bytes(bytes);
        if value < u32::MAX - u32::MAX % CODES {
            break value % CODES;
        }
    };
    let mut qr = [0; QR_SECRET_LEN];
    fill(&mut qr)?;
    Ok((format!("{code:06}"), qr))
}

impl HubHandle {
    async fn request<T>(&self, make: impl FnOnce(oneshot::Sender<T>) -> Command) -> Option<T> {
        let (reply, response) = oneshot::channel();
        self.commands.send(make(reply)).await.ok()?;
        response.await.ok()
    }

    async fn tell(&self, command: Command) {
        if self.commands.send(command).await.is_err() {
            log::debug!("hub stopped");
        }
    }

    pub async fn start_pairing(&self) -> anyhow::Result<(String, String)> {
        self.request(|reply| Command::StartPairing { reply })
            .await
            .unwrap_or_else(|| Err(anyhow::anyhow!("hub stopped")))
    }

    pub async fn cancel_pairing(&self) {
        self.tell(Command::CancelPairing).await;
    }

    pub async fn unpair(&self, id: DeviceId) -> bool {
        self.request(|reply| Command::Unpair { id, reply }).await.unwrap_or(false)
    }

    pub async fn admit(&self, spki: Spki) -> Admission {
        self.request(|reply| Command::Admit { spki, reply }).await.unwrap_or(Admission::Reject(CloseCode::Busy))
    }

    pub async fn paired(&self, spki: Spki, name: String, window: u64) {
        self.tell(Command::Paired { spki, name, window }).await;
    }

    pub async fn pairing_failed(&self, reason: String, window: u64) {
        self.tell(Command::PairingFailed { reason, window }).await;
    }

    pub async fn connected(&self, id: DeviceId, name: String, session: SessionHandle) {
        self.tell(Command::Connected { id, name, session }).await;
    }

    /// The device's live session, if it has one.
    pub async fn browse(&self, id: DeviceId, request: browse::Request) -> Result<browse::Answer, browse::Failure> {
        let (reply, answer) = oneshot::channel();
        if self.commands.send(Command::Browse { id, request, reply }).await.is_err() {
            return Err(browse::Failure::NotConnected);
        }
        answer.await.unwrap_or(Err(browse::Failure::NotConnected))
    }

    pub async fn session(&self, id: DeviceId) -> Option<SessionHandle> {
        self.request(|reply| Command::Session { id, reply }).await.flatten()
    }

    pub async fn set_grant(&self, id: DeviceId, feature: String, granted: bool) -> bool {
        self.request(|reply| Command::SetGrant { id, feature, granted, reply }).await.unwrap_or(false)
    }

    /// Connected devices holding the clipboard grant.
    pub async fn clipboard_peers(&self) -> Vec<DeviceId> {
        self.request(|reply| Command::ClipboardPeers { reply }).await.unwrap_or_default()
    }

    /// Offers files to a Link device or a `localsend:<fingerprint>` peer.
    pub async fn send_files(&self, device: String, sources: Vec<Source>) -> Result<TransferId, link_core::Error> {
        self.request(|reply| Command::SendFiles { device, sources, reply })
            .await
            .unwrap_or(Err(link_core::Error::Stopped))
    }

    /// Answers an offer from any backend; false when it no longer waits.
    pub async fn decide(&self, id: TransferId, accept: bool) -> bool {
        self.request(|reply| Command::Decide { id, accept, reply }).await.unwrap_or(false)
    }

    pub async fn cancel_transfer(&self, id: TransferId) -> bool {
        self.request(|reply| Command::CancelTransfer { id, reply }).await.unwrap_or(false)
    }

    pub async fn set_localsend(&self, visible: bool) {
        self.tell(Command::SetLocalSend { visible }).await;
    }

    pub async fn set_auto_accept(&self, id: DeviceId, enabled: bool) -> bool {
        self.request(|reply| Command::SetAutoAccept { id, enabled, reply }).await.unwrap_or(false)
    }

    pub fn transfers(&self) -> &TransferHandle {
        &self.transfers
    }

    /// Where a session delivers what its peer sends.
    pub fn route(&self, peer: DeviceId) -> Route {
        Route { peer, events: self.session_events.clone(), transfers: self.transfers.clone() }
    }

    pub async fn desktop_player(&self, player: MediaPlayer) {
        self.tell(Command::DesktopPlayer { player }).await;
    }

    pub async fn desktop_player_gone(&self, player: String) {
        self.tell(Command::DesktopPlayerGone { player }).await;
    }

    pub async fn disconnected(&self, id: DeviceId, stable_id: usize) {
        self.tell(Command::Disconnected { id, stable_id }).await;
    }
}
