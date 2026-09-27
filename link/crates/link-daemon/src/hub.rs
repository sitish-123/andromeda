//! The hub actor: sole owner of the device store, the pairing window, and the set of live sessions.

use std::collections::HashMap;
use std::sync::Arc;
use std::time::Duration;

use link_core::discovery::Advertiser;
use link_core::identity::{DeviceId, Spki};
use link_core::net;
use link_core::proto::CloseCode;
use link_core::proto::message::Share;
use link_core::proto::pairing::{Secret, Secrets};
use link_core::session::{Route, SessionEvent, SessionHandle};
use link_core::store::{Peer, Store};
use link_core::transfer::{TransferEvent, TransferHandle};
use link_core::uri::{PairingUri, QR_SECRET_LEN};
use ring::rand::{SecureRandom, SystemRandom};
use tokio::sync::{mpsc, oneshot, watch};
use tokio::time::Instant;

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
    /// Every transfer event but a `Busy` and an auto-accepted `Offered`.
    Transfer(TransferEvent),
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
    commands: mpsc::Receiver<Command>,
    session_events: mpsc::Receiver<SessionEvent>,
    snapshots: watch::Sender<Snapshot>,
    events: mpsc::Sender<Event>,
    transfers: TransferHandle,
    transfer_events: mpsc::UnboundedReceiver<TransferEvent>,
}

/// The transfer actor the hub consents for and attaches sessions to.
pub struct Transfers {
    pub handle: TransferHandle,
    pub events: mpsc::UnboundedReceiver<TransferEvent>,
}

impl Hub {
    pub fn new(
        own: Spki,
        store: Store,
        paths: Paths,
        transfers: Transfers,
    ) -> (Self, HubHandle, watch::Receiver<Snapshot>, mpsc::Receiver<Event>) {
        let (commands_tx, commands) = mpsc::channel(32);
        let (session_events_tx, session_events) = mpsc::channel(16);
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
            commands,
            session_events,
            snapshots,
            events,
            transfers: transfers.handle.clone(),
            transfer_events: transfers.events,
        };
        hub.refresh();
        let handle =
            HubHandle { commands: commands_tx, session_events: session_events_tx, transfers: transfers.handle };
        (hub, handle, snapshots_rx, events_rx)
    }

    pub async fn run(mut self) -> anyhow::Result<()> {
        loop {
            let deadline = self.window.as_ref().map(|window| window.deadline);
            tokio::select! {
                command = self.commands.recv() => match command {
                    Some(command) => self.handle(command).await,
                    None => return Ok(()),
                },
                Some(event) = self.session_events.recv() => self.on_session_event(event).await,
                Some(event) = self.transfer_events.recv() => self.on_transfer(event).await,
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
            Command::Unpair { id, reply } => drop(reply.send(self.unpair(&id))),
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
                    self.publish();
                }
            }
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
        }
    }

    /// Consent is the hub's: an offer from an auto-accept device is accepted here, any other goes to the shell.
    async fn on_transfer(&mut self, event: TransferEvent) {
        match &event {
            TransferEvent::Busy { .. } => return,
            TransferEvent::Offered { id, from, .. } if self.store.peer(from).is_some_and(|peer| peer.auto_accept) => {
                log::info!("{from}: auto-accepting transfer {id}");
                if let Err(error) = self.transfers.decide(*id, true).await {
                    log::error!("accepting {id}: {error}");
                }
                return;
            }
            _ => {}
        }
        self.emit(Event::Transfer(event)).await;
    }

    async fn on_session_event(&mut self, event: SessionEvent) {
        match event {
            SessionEvent::Received { from, share } => {
                if self.sessions.contains_key(&from) {
                    self.emit(Event::Received { id: from, share }).await;
                }
            }
            SessionEvent::Unpaired { from } => {
                log::info!("{from} unpaired itself");
                self.sessions.remove(&from);
                self.store.remove(&from);
                self.save();
                self.refresh();
            }
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
        if let Some(older) = self.sessions.insert(id.clone(), session.clone()) {
            older.close(CloseCode::Done);
        }
        self.publish();
        self.transfers.attach(id, session).await;
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
        self.snapshots.send_if_modified(|current| {
            let next = Snapshot { devices, pairing: self.window.is_some(), auto_accept };
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
    pub async fn session(&self, id: DeviceId) -> Option<SessionHandle> {
        self.request(|reply| Command::Session { id, reply }).await.flatten()
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

    pub async fn disconnected(&self, id: DeviceId, stable_id: usize) {
        self.tell(Command::Disconnected { id, stable_id }).await;
    }
}
