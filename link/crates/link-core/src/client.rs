//! The phone's actor over [`Phone`]: its live sessions, presence with reconnection, file transfers, and the events an
//! app shows. Shared by the headless phone and the Android bindings.

use std::collections::{HashMap, HashSet};
use std::net::SocketAddr;
use std::time::Duration;

use link_proto::CloseCode;
use link_proto::message::{FsRefusal, Hotspot, HotspotEnd, Message, NetworkKind, Share, Status, TransferId};
use link_proto::session::Role;
use tokio::sync::{mpsc, oneshot};
use tokio::task::JoinSet;
use tokio::time::Instant;

use crate::browse;
use crate::hotspot::{self, UPGRADE_TIMEOUT};
use crate::identity::DeviceId;
use crate::inbox::Inbox;
use crate::phone::{self, PairTarget, Phone};
use crate::reach::{self, Reached, Via};
use crate::session::{self, Route, SessionEvent, SessionHandle};
use crate::store::{Feature, Peer, feature_of};
use crate::stream::BLUETOOTH_FILE_LIMIT;
use crate::tls::ServerPin;
use crate::transfer::{self, LocalClip, Source, TransferActor, TransferEvent, TransferHandle};
use crate::{Error, close_code_for};

const FIRST_RETRY: Duration = Duration::from_secs(1);
const MAX_RETRY: Duration = Duration::from_secs(30);
/// The least time between two status messages; a desktop drops more than three in 30 s.
const STATUS_EVERY: Duration = Duration::from_secs(10);
/// How often a session on Bluetooth looks for an IP path to move to.
const PROBE_EVERY: Duration = Duration::from_secs(30);

#[derive(Debug, Clone)]
pub enum ClientEvent {
    Connected {
        desktop: Peer,
        /// None over Bluetooth.
        addr: Option<SocketAddr>,
        via: Via,
        resumed: bool,
    },
    Disconnected {
        id: DeviceId,
        reason: String,
    },
    Received {
        from: DeviceId,
        share: Share,
    },
    /// The desktop unpaired this phone, which has forgotten it.
    Unpaired {
        id: DeviceId,
    },
    /// Offers, progress, and results of file transfers in both directions.
    Transfer(TransferEvent),
    /// A feature message from a desktop whose switch for that feature is on.
    Message {
        from: DeviceId,
        message: Message,
    },
}

#[derive(Debug, Clone)]
pub struct DesktopState {
    pub peer: Peer,
    pub connected: bool,
    /// The live session runs over Bluetooth.
    pub bluetooth: bool,
}

enum Command {
    Keep {
        id: DeviceId,
        keep: bool,
    },
    SetStatus {
        status: Status,
    },
    Pair {
        target: PairTarget,
        reply: oneshot::Sender<Result<Peer, Error>>,
    },
    Session {
        id: DeviceId,
        reply: oneshot::Sender<Result<SessionHandle, Error>>,
    },
    SetPresent {
        present: bool,
    },
    Forget {
        id: DeviceId,
        reply: oneshot::Sender<Result<(), Error>>,
    },
    Desktops {
        reply: oneshot::Sender<Vec<DesktopState>>,
    },
    SetSharing {
        id: DeviceId,
        feature: Feature,
        on: bool,
        reply: oneshot::Sender<Result<(), Error>>,
    },
    SetBrowseRoots {
        roots: browse::Roots,
    },
    /// Moves `id`'s Bluetooth session to this phone's hotspot; answered once it runs over IP, or with why not.
    Upgrade {
        id: DeviceId,
        reply: oneshot::Sender<Result<(), Error>>,
    },
    /// The live sessions of desktops whose switch for `feature` is on.
    Sessions {
        feature: Feature,
        reply: oneshot::Sender<Vec<SessionHandle>>,
    },
}

/// The handle apps hold; every method is answered by the [`ClientActor`].
#[derive(Clone)]
pub struct Client {
    commands: mpsc::Sender<Command>,
    transfers: TransferHandle,
}

pub struct ClientActor {
    phone: Phone,
    commands: mpsc::Receiver<Command>,
    events: mpsc::Sender<ClientEvent>,
    sessions: HashMap<DeviceId, SessionHandle>,
    running: JoinSet<Ended>,
    session_events_tx: mpsc::Sender<SessionEvent>,
    session_events: mpsc::Receiver<SessionEvent>,
    transfers: TransferHandle,
    transfer_actor: Option<TransferActor>,
    transfer_events: mpsc::UnboundedReceiver<TransferEvent>,
    present: bool,
    /// Desktops with an open transfer, kept connected like a present phone until it ends.
    busy: HashSet<DeviceId>,
    /// Desktops a transfer is being started with, kept like busy ones until the transfer actor reports them.
    keeping: HashSet<DeviceId>,
    retries: HashMap<DeviceId, Retry>,
    status: Option<Status>,
    /// The status last sent, when, and whether a newer one waits for [`STATUS_EVERY`] to pass.
    status_sent: Option<Instant>,
    status_pending: bool,
    /// Desktops whose live session runs over Bluetooth, probed for an IP path every [`PROBE_EVERY`].
    on_bluetooth: HashSet<DeviceId>,
    next_probe: Option<Instant>,
    probes: JoinSet<(DeviceId, Result<Reached, Error>)>,
    /// The desktop the phone's hotspot runs for; one at a time.
    hotspot_for: Option<DeviceId>,
    /// Desktops whose live session runs over the phone's hotspot.
    on_hotspot: HashSet<DeviceId>,
    hotspot_started: JoinSet<(DeviceId, std::io::Result<Hotspot>)>,
    /// When the hotspot stops unless a transfer starts first.
    hotspot_idle_at: Option<Instant>,
    upgrades: HashMap<DeviceId, Vec<oneshot::Sender<Result<(), Error>>>>,
    /// Browse requests being answered, at most [`browse::MAX_IN_FLIGHT`].
    browsing: JoinSet<()>,
}

struct Ended {
    id: DeviceId,
    stable_id: usize,
    result: Result<(), Error>,
}

struct Retry {
    at: Instant,
    delay: Duration,
}

/// The caller runs the actor in a task it owns and reads the events. Received files go to `inbox`.
pub fn client(phone: Phone, inbox: Inbox) -> (Client, ClientActor, mpsc::Receiver<ClientEvent>) {
    let (commands_tx, commands) = mpsc::channel(16);
    let (events, events_rx) = mpsc::channel(64);
    let (session_events_tx, session_events) = mpsc::channel(16);
    let (transfers, transfer_actor, transfer_events) = transfer::transfers(inbox);
    let actor = ClientActor {
        phone,
        commands,
        events,
        sessions: HashMap::new(),
        running: JoinSet::new(),
        session_events_tx,
        session_events,
        transfers: transfers.clone(),
        transfer_actor: Some(transfer_actor),
        transfer_events,
        present: false,
        busy: HashSet::new(),
        keeping: HashSet::new(),
        retries: HashMap::new(),
        status: None,
        status_sent: None,
        status_pending: false,
        on_bluetooth: HashSet::new(),
        next_probe: None,
        probes: JoinSet::new(),
        hotspot_for: None,
        on_hotspot: HashSet::new(),
        hotspot_started: JoinSet::new(),
        hotspot_idle_at: None,
        upgrades: HashMap::new(),
        browsing: JoinSet::new(),
    };
    (Client { commands: commands_tx, transfers }, actor, events_rx)
}

impl Client {
    /// Pairs and keeps the session open.
    pub async fn pair(&self, target: PairTarget) -> Result<Peer, Error> {
        self.request(|reply| Command::Pair { target, reply }).await?
    }

    /// Opens a session unless one is live.
    pub async fn connect(&self, id: DeviceId) -> Result<(), Error> {
        self.session(id).await.map(drop)
    }

    /// While present, every paired desktop is kept connected with keep-alive and redialled when it drops; otherwise
    /// sessions are opened on demand and idle out.
    pub async fn set_present(&self, present: bool) -> Result<(), Error> {
        self.commands.send(Command::SetPresent { present }).await.map_err(|_| Error::Stopped)
    }

    /// Sends `share`, connecting first if needed, and waits for the desktop's ack.
    pub async fn share(&self, id: DeviceId, share: Share) -> Result<(), Error> {
        share.check()?;
        self.session(id).await?.share(share).await
    }

    /// Offers files to the desktop, connecting with keep-alive first; the session stays until the transfer ends.
    pub async fn send_files(&self, id: DeviceId, sources: Vec<Source>) -> Result<TransferId, Error> {
        self.commands.send(Command::Keep { id: id.clone(), keep: true }).await.map_err(|_| Error::Stopped)?;
        let total: u64 = sources.iter().map(Source::size).sum();
        let sent = match self.session(id.clone()).await {
            Ok(handle) if handle.connection().quic().is_none() && total > BLUETOOTH_FILE_LIMIT => {
                match self.upgrade(id.clone()).await {
                    Ok(()) => self.transfers.send(id.clone(), sources).await,
                    Err(error) => Err(error),
                }
            }
            Ok(_) => self.transfers.send(id.clone(), sources).await,
            Err(error) => Err(error),
        };
        self.commands.send(Command::Keep { id, keep: false }).await.map_err(|_| Error::Stopped)?;
        sent
    }

    /// Offers the phone's clipboard to every connected desktop whose clipboard switch is on.
    pub async fn offer_clip(&self, clip: LocalClip) -> Result<(), Error> {
        let desktops = self.desktops().await?;
        let connected = desktops
            .into_iter()
            .filter(|desktop| desktop.connected && desktop.peer.grants.clipboard)
            .map(|desktop| desktop.peer.id);
        self.transfers.offer_clip(connected.collect(), clip).await
    }

    /// Writes `mime` of a desktop's clipboard offer into `sink`.
    pub async fn pull_clip(&self, id: DeviceId, clip: u64, mime: String, sink: std::fs::File) -> Result<u64, Error> {
        self.transfers.pull_clip(id, clip, mime, sink).await
    }

    /// The folders a desktop with the browse switch on sees; empty while the app may not read shared storage.
    pub async fn set_browse_roots(&self, roots: browse::Roots) -> Result<(), Error> {
        self.commands.send(Command::SetBrowseRoots { roots }).await.map_err(|_| Error::Stopped)
    }

    /// The phone's battery and network: sent to every desktop on connect and, at most every 10 s, on change.
    pub async fn set_status(&self, status: Status) -> Result<(), Error> {
        self.commands.send(Command::SetStatus { status }).await.map_err(|_| Error::Stopped)
    }

    /// Answers a desktop's offer; false when it no longer waits.
    pub async fn decide(&self, transfer: TransferId, accept: bool) -> Result<bool, Error> {
        self.transfers.decide(transfer, accept).await
    }

    pub async fn cancel_transfer(&self, transfer: TransferId) -> Result<bool, Error> {
        self.transfers.cancel(transfer).await
    }

    /// Tells the desktop, then forgets it. Returns whether the desktop was told; it is forgotten either way.
    pub async fn unpair(&self, id: DeviceId) -> Result<bool, Error> {
        let told = match self.session(id.clone()).await {
            Ok(handle) => handle.unpair().await,
            Err(error) => Err(error),
        };
        if let Err(error) = &told {
            log::info!("unpairing {id} without telling it: {error}");
        }
        self.request(|reply| Command::Forget { id, reply }).await??;
        Ok(told.is_ok())
    }

    pub async fn desktops(&self) -> Result<Vec<DesktopState>, Error> {
        self.request(|reply| Command::Desktops { reply }).await
    }

    pub async fn set_sharing(&self, id: DeviceId, feature: Feature, on: bool) -> Result<(), Error> {
        self.request(|reply| Command::SetSharing { id, feature, on, reply }).await?
    }

    /// Sends an unacknowledged message to every connected desktop whose switch for its feature is on, without
    /// dialling; returns how many it was written to.
    pub async fn broadcast(&self, message: Message) -> Result<usize, Error> {
        message.validate()?;
        let feature = feature_of(&message).ok_or(Error::Unexpected(message.kind()))?;
        let sessions = self.request(|reply| Command::Sessions { feature, reply }).await?;
        let mut sent = 0;
        for session in sessions {
            match session.send(message.clone()).await {
                Ok(()) => sent += 1,
                Err(error) => log::info!("broadcasting a {}: {error}", message.kind()),
            }
        }
        Ok(sent)
    }

    /// Sends an unacknowledged message to one desktop, connecting first if needed.
    pub async fn send(&self, id: DeviceId, message: Message) -> Result<(), Error> {
        message.validate()?;
        self.session(id).await?.send(message).await
    }

    /// Waits for `id`'s session to move from Bluetooth to the phone's hotspot.
    async fn upgrade(&self, id: DeviceId) -> Result<(), Error> {
        let upgraded = self.request(|reply| Command::Upgrade { id, reply });
        tokio::time::timeout(UPGRADE_TIMEOUT, upgraded).await.map_err(|_| Error::TooLargeForBluetooth)??
    }

    async fn session(&self, id: DeviceId) -> Result<SessionHandle, Error> {
        self.request(|reply| Command::Session { id, reply }).await?
    }

    async fn request<T>(&self, make: impl FnOnce(oneshot::Sender<T>) -> Command) -> Result<T, Error> {
        let (reply, response) = oneshot::channel();
        self.commands.send(make(reply)).await.map_err(|_| Error::Stopped)?;
        response.await.map_err(|_| Error::Stopped)
    }
}

impl ClientActor {
    /// Runs until every [`Client`] is dropped, then closes the sessions and waits for the closes to be sent.
    pub async fn run(mut self) {
        let Some(transfer_actor) = self.transfer_actor.take() else { return };
        tokio::select! {
            () = self.serve() => {}
            () = transfer_actor.run() => log::error!("the transfer actor stopped"),
        }
        for handle in self.sessions.values() {
            handle.close(CloseCode::Done);
        }
        self.phone.finish().await;
    }

    async fn serve(&mut self) {
        loop {
            let next_retry = self.retries.values().map(|retry| retry.at).min();
            let next_status = self.status_pending.then(|| self.status_sent.map(|sent| sent + STATUS_EVERY)).flatten();
            tokio::select! {
                command = self.commands.recv() => match command {
                    Some(command) => self.handle(command).await,
                    None => return,
                },
                Some(joined) = self.running.join_next() => match joined {
                    Ok(ended) => self.ended(ended).await,
                    Err(join) => log::warn!("session task: {join}"),
                },
                Some(event) = self.session_events.recv() => self.relay(event).await,
                Some(event) = self.transfer_events.recv() => self.on_transfer(event).await,
                () = sleep_until(next_retry) => self.retry_due().await,
                () = sleep_until(next_status) => self.send_status().await,
                () = sleep_until(self.next_probe) => self.probe(),
                Some(_) = self.browsing.join_next() => {}
                () = sleep_until(self.hotspot_idle_at) => self.end_hotspot(None).await,
                Some(joined) = self.hotspot_started.join_next() => match joined {
                    Ok((id, started)) => self.hotspot_up(id, started).await,
                    Err(join) => log::warn!("hotspot task: {join}"),
                },
                Some(joined) = self.probes.join_next() => match joined {
                    Ok((id, reached)) => self.probed(id, reached).await,
                    Err(join) => log::warn!("probe task: {join}"),
                },
            }
        }
    }

    async fn handle(&mut self, command: Command) {
        match command {
            Command::Pair { target, reply } => {
                let result = self.pair(target).await;
                drop(reply.send(result));
            }
            Command::Session { id, reply } => {
                let result = self.live_session(&id).await;
                drop(reply.send(result));
            }
            Command::SetPresent { present } => self.set_present(present),
            Command::Forget { id, reply } => {
                self.retries.remove(&id);
                if let Some(handle) = self.sessions.remove(&id) {
                    handle.close(CloseCode::Done);
                }
                drop(reply.send(self.phone.forget(&id)));
            }
            Command::Desktops { reply } => drop(reply.send(self.desktops())),
            Command::SetStatus { status } => {
                let before = self.status.map(|status| status.network);
                if before != Some(status.network) {
                    self.network_changed(before, status.network).await;
                }
                if self.status != Some(status) {
                    self.status = Some(status);
                    self.status_pending = true;
                    if self.status_sent.is_none_or(|sent| sent.elapsed() >= STATUS_EVERY) {
                        self.send_status().await;
                    }
                }
            }
            Command::Keep { id, keep } => {
                if keep {
                    self.keeping.insert(id);
                } else {
                    self.keeping.remove(&id);
                }
            }
            Command::SetSharing { id, feature, on, reply } => {
                drop(reply.send(self.phone.set_sharing(&id, feature, on)));
            }
            Command::SetBrowseRoots { roots } => self.phone.set_browse_roots(roots),
            Command::Upgrade { id, reply } => {
                self.upgrades.entry(id.clone()).or_default().push(reply);
                self.start_hotspot(id).await;
            }
            Command::Sessions { feature, reply } => {
                let sessions = self
                    .phone
                    .desktops()
                    .iter()
                    .filter(|peer| peer.grants.allows(feature))
                    .filter_map(|peer| self.sessions.get(&peer.id).filter(|handle| handle.is_live()).cloned())
                    .collect();
                drop(reply.send(sessions));
            }
        }
    }

    async fn on_transfer(&mut self, event: TransferEvent) {
        if let TransferEvent::Busy { peer, busy } = &event {
            if self.hotspot_for.as_ref() == Some(peer) {
                self.hotspot_idle_at = (!*busy).then(|| Instant::now() + hotspot::IDLE);
            }
            if *busy {
                self.busy.insert(peer.clone());
            } else {
                self.busy.remove(peer);
                if !self.present
                    && let Some(handle) = self.sessions.get(peer)
                {
                    handle.close(CloseCode::Done);
                }
            }
            return;
        }
        let allows = |id: &DeviceId, feature| {
            self.phone.desktops().iter().any(|peer| peer.id == *id && peer.grants.allows(feature))
        };
        match &event {
            TransferEvent::ClipOffered { from, .. } if !allows(from, Feature::Clipboard) => {
                log::info!("{from}: dropped a clipboard offer its switch does not allow");
                return;
            }
            TransferEvent::Offered { id, from, .. } if !allows(from, Feature::Files) => {
                log::info!("{from}: declining transfer {id}: its files switch is off");
                if let Err(error) = self.transfers.decide(*id, false).await {
                    log::warn!("declining {id}: {error}");
                }
                return;
            }
            _ => {}
        }
        self.emit(ClientEvent::Transfer(event)).await;
    }

    async fn pair(&mut self, target: PairTarget) -> Result<Peer, Error> {
        let session = self.phone.pair(target).await?;
        let peer = session.desktop.clone();
        self.adopt(session).await;
        Ok(peer)
    }

    async fn live_session(&mut self, id: &DeviceId) -> Result<SessionHandle, Error> {
        if let Some(handle) = self.sessions.get(id).filter(|handle| handle.is_live()) {
            return Ok(handle.clone());
        }
        self.open(id).await
    }

    async fn open(&mut self, id: &DeviceId) -> Result<SessionHandle, Error> {
        self.phone.set_present(self.present || self.is_busy(id));
        match self.phone.connect(id).await {
            Ok(session) => Ok(self.adopt(session).await),
            Err(error) => {
                if matches!(error, Error::Closed(CloseCode::Unpaired)) {
                    self.retries.remove(id);
                    self.emit(ClientEvent::Unpaired { id: id.clone() }).await;
                }
                Err(error)
            }
        }
    }

    async fn adopt(&mut self, session: phone::Session) -> SessionHandle {
        let phone::Session { connection, control, desktop, addr, via, resumed } = session;
        let id = desktop.id.clone();
        let route =
            Route { peer: id.clone(), events: self.session_events_tx.clone(), transfers: self.transfers.clone() };
        let (handle, actor) = session::session(connection, control, Role::Phone, route);
        let (task_id, stable_id) = (id.clone(), handle.stable_id());
        self.running.spawn(async move { Ended { id: task_id, stable_id, result: actor.run().await } });
        if let Some(old) = self.sessions.insert(id.clone(), handle.clone()) {
            old.close(CloseCode::Done);
        }
        self.retries.remove(&id);
        if via == Via::Bluetooth {
            log::info!("{id}: connected over Bluetooth");
            self.on_bluetooth.insert(id.clone());
            self.on_hotspot.remove(&id);
            self.next_probe.get_or_insert_with(|| Instant::now() + PROBE_EVERY);
        } else {
            log::info!("{id}: connected at {}", addr.map_or_else(String::new, |addr| addr.to_string()));
            self.on_bluetooth.remove(&id);
            if via == Via::Hotspot {
                self.on_hotspot.insert(id.clone());
            } else {
                self.on_hotspot.remove(&id);
            }
            self.finish_upgrades(&id, &Ok(()));
            if self.hotspot_for.as_ref() == Some(&id) && !self.is_busy(&id) {
                self.hotspot_idle_at = Some(Instant::now() + hotspot::IDLE);
            }
        }
        if let Some(status) = self.status {
            let (handle, status) = (handle.clone(), Message::Status(status));
            if let Err(error) = handle.send(status).await {
                log::info!("{id}: sending the status: {error}");
            }
        }
        self.emit(ClientEvent::Connected { desktop, addr, via, resumed }).await;
        self.transfers.attach(id, handle.clone()).await;
        handle
    }

    async fn ended(&mut self, ended: Ended) {
        let Ended { id, stable_id, result } = ended;
        self.transfers.detach(id.clone(), stable_id).await;
        if self.sessions.get(&id).is_none_or(|handle| handle.stable_id() != stable_id) {
            return;
        }
        let Some(handle) = self.sessions.remove(&id) else { return };
        self.on_bluetooth.remove(&id);
        self.on_hotspot.remove(&id);
        if self.hotspot_for.as_ref() == Some(&id) {
            self.stop_hotspot();
        }
        let reason = match &result {
            Ok(()) => "done".to_owned(),
            Err(error) => error.to_string(),
        };
        if let Err(error) = &result
            && !matches!(error, Error::Timeout | Error::Closed(_))
        {
            handle.close(close_code_for(error));
        }
        log::info!("{id}: disconnected: {reason}");
        self.emit(ClientEvent::Disconnected { id: id.clone(), reason }).await;
        if matches!(result, Err(Error::Closed(CloseCode::Unpaired))) {
            if let Err(error) = self.phone.forget(&id) {
                log::error!("forgetting {id}: {error}");
            }
            self.emit(ClientEvent::Unpaired { id }).await;
        } else if self.wants(&id) {
            self.retries.insert(id, Retry { at: Instant::now() + FIRST_RETRY, delay: FIRST_RETRY });
        }
    }

    async fn retry_due(&mut self) {
        let now = Instant::now();
        let due: Vec<DeviceId> =
            self.retries.iter().filter(|(_, retry)| retry.at <= now).map(|(id, _)| id.clone()).collect();
        for id in due {
            let Some(retry) = self.retries.remove(&id) else { continue };
            if self.sessions.get(&id).is_some_and(SessionHandle::is_live) {
                continue;
            }
            let Err(error) = self.open(&id).await else { continue };
            if self.wants(&id) {
                let delay = (retry.delay * 2).min(MAX_RETRY);
                log::info!("{id}: redial failed: {error}; next in {} s", delay.as_secs());
                self.retries.insert(id, Retry { at: Instant::now() + delay, delay });
            }
        }
    }

    /// Wi-Fi going away moves every session over it to Bluetooth at once, instead of after the idle timeout and a
    /// round of redials; Wi-Fi coming back looks for an IP path at once, instead of at the next probe.
    async fn network_changed(&mut self, before: Option<NetworkKind>, now: NetworkKind) {
        if before == Some(NetworkKind::Wifi) && now != NetworkKind::Wifi {
            let over_wifi: Vec<Peer> = self
                .phone
                .desktops()
                .iter()
                .filter(|peer| peer.bluetooth.is_some() && !self.on_bluetooth.contains(&peer.id) && self.wants(&peer.id))
                .cloned()
                .collect();
            for peer in over_wifi {
                log::info!("{}: Wi-Fi is gone; moving to Bluetooth", peer.id);
                match self.phone.connect_bluetooth(&peer).await {
                    Ok(session) => drop(self.adopt(session).await),
                    Err(error) => log::info!("{}: Bluetooth right after Wi-Fi: {error}", peer.id),
                }
            }
        } else if now == NetworkKind::Wifi && !self.on_bluetooth.is_empty() {
            self.next_probe = Some(Instant::now());
        }
    }

    /// Looks for an IP path to every desktop on Bluetooth, off the actor, since reaching one can take seconds.
    fn probe(&mut self) {
        self.next_probe = (!self.on_bluetooth.is_empty()).then(|| Instant::now() + PROBE_EVERY);
        for id in &self.on_bluetooth {
            let Some(peer) = self.phone.desktops().iter().find(|peer| peer.id == *id).cloned() else { continue };
            let dialer = self.phone.dialer().clone();
            let id = id.clone();
            self.probes.spawn(async move {
                let reached = reach::reach(&dialer, &peer).await;
                (id, reached)
            });
        }
    }

    /// Moves a Bluetooth session to the IP path a probe found; the desktop closes the older session.
    async fn probed(&mut self, id: DeviceId, reached: Result<Reached, Error>) {
        let Ok(reached) = reached else { return };
        if !self.on_bluetooth.contains(&id) {
            return;
        }
        let Some(peer) = self.phone.desktops().iter().find(|peer| peer.id == id).cloned() else { return };
        match self.phone.adopt_reached(&peer, reached).await {
            Ok(session) => {
                log::info!("{id}: moving from Bluetooth to {:?}", session.addr);
                self.adopt(session).await;
            }
            Err(error) => log::info!("{id}: the IP path a probe found failed: {error}"),
        }
    }

    /// Starts the hotspot for `id`'s Bluetooth session, off the actor; its credentials go out once it is up.
    async fn start_hotspot(&mut self, id: DeviceId) {
        if !self.on_bluetooth.contains(&id) {
            self.finish_upgrades(&id, &Ok(()));
            return;
        }
        if self.hotspot_for.as_ref() == Some(&id) {
            return;
        }
        let files = self.phone.desktops().iter().any(|peer| peer.id == id && peer.grants.files);
        let refusal = match (&self.hotspot_for, self.phone.hotspot()) {
            _ if !files => Some("its files switch is off"),
            (Some(_), _) => Some("the hotspot serves another desktop"),
            (None, None) => Some("this phone cannot start a hotspot"),
            (None, Some(provider)) => {
                self.hotspot_for = Some(id.clone());
                let for_id = id.clone();
                self.hotspot_started.spawn_blocking(move || (for_id, provider.start()));
                None
            }
        };
        if let Some(reason) = refusal {
            self.tell_hotspot_end(&id, Some(reason)).await;
            self.finish_upgrades(&id, &Err(Error::TooLargeForBluetooth));
        }
    }

    async fn hotspot_up(&mut self, id: DeviceId, started: std::io::Result<Hotspot>) {
        if self.hotspot_for.as_ref() != Some(&id) {
            if started.is_ok() {
                self.stop_hotspot_provider();
            }
            return;
        }
        let sent = match started {
            Ok(hotspot) => match self.sessions.get(&id) {
                Some(handle) => handle.send(Message::Hotspot(hotspot)).await.map_err(|error| error.to_string()),
                None => Err("the Bluetooth session ended".to_owned()),
            },
            Err(error) => {
                self.hotspot_for = None;
                Err(error.to_string())
            }
        };
        if let Err(reason) = sent {
            log::info!("{id}: no hotspot: {reason}");
            self.stop_hotspot();
            self.tell_hotspot_end(&id, Some(&reason)).await;
            self.finish_upgrades(&id, &Err(Error::Hotspot(reason)));
        }
    }

    /// The desktop joined the hotspot; dialling the address it reported moves the session there.
    fn hotspot_joined(&mut self, id: DeviceId, joined: &link_proto::message::HotspotJoined) {
        let (Some(peer), Ok(addr)) =
            (self.phone.desktops().iter().find(|peer| peer.id == id).cloned(), joined.address.parse())
        else {
            return;
        };
        if self.hotspot_for.as_ref() != Some(&id) {
            return;
        }
        let dialer = self.phone.dialer().clone();
        self.probes.spawn(async move {
            let reached = async {
                let pin = ServerPin::Key(peer.spki()?.fingerprint());
                let (connection, addr) = reach::race(&dialer, &[addr], pin).await?;
                Ok(Reached { dialed: connection, addr, via: Via::Hotspot })
            };
            (id, reached.await)
        });
    }

    /// Nothing moved through the hotspot for [`hotspot::IDLE`], or it failed: the desktop is told, then it stops.
    async fn end_hotspot(&mut self, reason: Option<&str>) {
        if let Some(id) = self.hotspot_for.clone() {
            self.tell_hotspot_end(&id, reason).await;
        }
        self.stop_hotspot();
    }

    async fn tell_hotspot_end(&self, id: &DeviceId, reason: Option<&str>) {
        let Some(handle) = self.sessions.get(id) else { return };
        let reason = reason.map(|reason| reason.chars().take(link_proto::message::MAX_REASON_LEN).collect());
        if let Err(error) = handle.send(Message::HotspotEnd(HotspotEnd { reason })).await {
            log::info!("{id}: telling the hotspot ended: {error}");
        }
    }

    /// Stops the hotspot. A session running over it is closed at once, so the redial falls back to Bluetooth now
    /// instead of after the idle timeout.
    fn stop_hotspot(&mut self) {
        self.hotspot_idle_at = None;
        let Some(id) = self.hotspot_for.take() else { return };
        self.stop_hotspot_provider();
        if self.on_hotspot.contains(&id)
            && let Some(handle) = self.sessions.get(&id)
        {
            handle.close(CloseCode::Done);
        }
    }

    fn stop_hotspot_provider(&self) {
        if let Some(provider) = self.phone.hotspot() {
            drop(tokio::task::spawn_blocking(move || provider.stop()));
        }
    }

    fn finish_upgrades(&mut self, id: &DeviceId, result: &Result<(), Error>) {
        for reply in self.upgrades.remove(id).unwrap_or_default() {
            let result = match result {
                Ok(()) => Ok(()),
                Err(Error::Hotspot(reason)) => Err(Error::Hotspot(reason.clone())),
                Err(_) => Err(Error::TooLargeForBluetooth),
            };
            drop(reply.send(result));
        }
    }

    /// Answers a browse request off the actor, when this desktop's browse switch is on.
    async fn browse(&mut self, from: &DeviceId, request: Message) {
        let Some(handle) = self.sessions.get(from).cloned() else { return };
        let req = match &request {
            Message::FsList(list) => list.req,
            Message::FsRead(read) => read.req,
            _ => return,
        };
        let allowed = self.phone.desktops().iter().any(|peer| peer.id == *from && peer.grants.browse);
        let refusal = if !allowed {
            Some(FsRefusal::NotAllowed)
        } else if self.phone.browse_roots().is_empty() {
            Some(FsRefusal::Denied)
        } else if self.browsing.len() >= browse::MAX_IN_FLIGHT {
            Some(FsRefusal::Busy)
        } else {
            None
        };
        match refusal {
            Some(reason) => {
                if let Err(error) = handle.send(browse::error(req, reason)).await {
                    log::info!("{from}: refusing a browse request: {error}");
                }
            }
            None => drop(self.browsing.spawn(browse::answer(self.phone.browse_roots().clone(), handle, request))),
        }
    }

    fn set_present(&mut self, present: bool) {
        self.present = present;
        if !present {
            let (busy, keeping) = (&self.busy, &self.keeping);
            self.retries.retain(|id, _| busy.contains(id) || keeping.contains(id));
            for (id, handle) in &self.sessions {
                if !self.is_busy(id) {
                    handle.close(CloseCode::Done);
                }
            }
            return;
        }
        let now = Instant::now();
        for peer in self.phone.desktops() {
            if !self.sessions.get(&peer.id).is_some_and(SessionHandle::is_live) {
                self.retries.entry(peer.id.clone()).or_insert(Retry { at: now, delay: FIRST_RETRY });
            }
        }
    }

    /// Sends the latest status to every live session.
    async fn send_status(&mut self) {
        self.status_pending = false;
        self.status_sent = Some(Instant::now());
        let Some(status) = self.status else { return };
        for (id, handle) in self.sessions.iter().filter(|(_, handle)| handle.is_live()) {
            if let Err(error) = handle.send(Message::Status(status)).await {
                log::info!("{id}: sending the status: {error}");
            }
        }
    }

    fn is_busy(&self, id: &DeviceId) -> bool {
        self.busy.contains(id) || self.keeping.contains(id)
    }

    /// Whether a lost session to `id` should be redialled.
    fn wants(&self, id: &DeviceId) -> bool {
        (self.present || self.is_busy(id)) && self.phone.desktops().iter().any(|peer| peer.id == *id)
    }

    async fn relay(&mut self, event: SessionEvent) {
        match event {
            SessionEvent::Message { from, message: Message::HotspotRequest } => self.start_hotspot(from).await,
            SessionEvent::Message { from, message: request @ (Message::FsList(_) | Message::FsRead(_)) } => {
                self.browse(&from, request).await;
            }
            SessionEvent::Message { from, message: Message::HotspotJoined(joined) } => {
                self.hotspot_joined(from, &joined);
            }
            SessionEvent::Message { from, message: Message::HotspotEnd(end) } => {
                if self.hotspot_for.as_ref() == Some(&from) {
                    log::info!("{from}: the desktop ended the hotspot: {:?}", end.reason);
                    self.stop_hotspot();
                    let reason = end.reason.unwrap_or_else(|| "the desktop left it".to_owned());
                    self.finish_upgrades(&from, &Err(Error::Hotspot(reason)));
                }
            }
            SessionEvent::Received { from, share } => self.emit(ClientEvent::Received { from, share }).await,
            SessionEvent::Message { from, message } => {
                let allowed = feature_of(&message).is_some_and(|feature| {
                    self.phone.desktops().iter().any(|peer| peer.id == from && peer.grants.allows(feature))
                });
                if allowed {
                    self.emit(ClientEvent::Message { from, message }).await;
                } else {
                    log::info!("{from}: dropped a {} its switch does not allow", message.kind());
                }
            }
            SessionEvent::Unpaired { .. } | SessionEvent::Status { .. } => {}
        }
    }

    fn desktops(&self) -> Vec<DesktopState> {
        let live = |id: &DeviceId| self.sessions.get(id).is_some_and(SessionHandle::is_live);
        self.phone
            .desktops()
            .iter()
            .map(|peer| DesktopState {
                peer: peer.clone(),
                connected: live(&peer.id),
                bluetooth: live(&peer.id) && self.on_bluetooth.contains(&peer.id),
            })
            .collect()
    }

    async fn emit(&self, event: ClientEvent) {
        if self.events.send(event).await.is_err() {
            log::debug!("nobody reads client events");
        }
    }
}

async fn sleep_until(deadline: Option<Instant>) {
    match deadline {
        Some(deadline) => tokio::time::sleep_until(deadline).await,
        None => std::future::pending().await,
    }
}
