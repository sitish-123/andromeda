//! The phone's actor over [`Phone`]: its live sessions, presence with reconnection, file transfers, and the events an
//! app shows. Shared by the headless phone and the Android bindings.

use std::collections::{HashMap, HashSet};
use std::net::SocketAddr;
use std::time::Duration;

use link_proto::CloseCode;
use link_proto::message::{Share, TransferId};
use link_proto::session::Role;
use tokio::sync::{mpsc, oneshot};
use tokio::task::JoinSet;
use tokio::time::Instant;

use crate::identity::DeviceId;
use crate::inbox::Inbox;
use crate::phone::{self, PairTarget, Phone};
use crate::reach::Via;
use crate::session::{self, Route, SessionEvent, SessionHandle};
use crate::store::Peer;
use crate::transfer::{self, LocalClip, Source, TransferActor, TransferEvent, TransferHandle};
use crate::{Error, close_code_for};

const FIRST_RETRY: Duration = Duration::from_secs(1);
const MAX_RETRY: Duration = Duration::from_secs(30);

#[derive(Debug, Clone)]
pub enum ClientEvent {
    Connected {
        desktop: Peer,
        addr: SocketAddr,
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
}

#[derive(Debug, Clone)]
pub struct DesktopState {
    pub peer: Peer,
    pub connected: bool,
}

enum Command {
    Pair { target: PairTarget, reply: oneshot::Sender<Result<Peer, Error>> },
    Session { id: DeviceId, reply: oneshot::Sender<Result<SessionHandle, Error>> },
    SetPresent { present: bool },
    Forget { id: DeviceId, reply: oneshot::Sender<Result<(), Error>> },
    Desktops { reply: oneshot::Sender<Vec<DesktopState>> },
    Keep { id: DeviceId, keep: bool },
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
        let sent = match self.session(id.clone()).await {
            Ok(_) => self.transfers.send(id.clone(), sources).await,
            Err(error) => Err(error),
        };
        self.commands.send(Command::Keep { id, keep: false }).await.map_err(|_| Error::Stopped)?;
        sent
    }

    /// Offers the phone's clipboard to every connected desktop.
    pub async fn offer_clip(&self, clip: LocalClip) -> Result<(), Error> {
        let desktops = self.desktops().await?;
        let connected = desktops.into_iter().filter(|desktop| desktop.connected).map(|desktop| desktop.peer.id);
        self.transfers.offer_clip(connected.collect(), clip).await
    }

    /// Writes `mime` of a desktop's clipboard offer into `sink`.
    pub async fn pull_clip(&self, id: DeviceId, clip: u64, mime: String, sink: std::fs::File) -> Result<u64, Error> {
        self.transfers.pull_clip(id, clip, mime, sink).await
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
            Command::Keep { id, keep } => {
                if keep {
                    self.keeping.insert(id);
                } else {
                    self.keeping.remove(&id);
                }
            }
        }
    }

    async fn on_transfer(&mut self, event: TransferEvent) {
        if let TransferEvent::Busy { peer, busy } = &event {
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
        log::info!("{id}: connected at {addr}");
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

    fn is_busy(&self, id: &DeviceId) -> bool {
        self.busy.contains(id) || self.keeping.contains(id)
    }

    /// Whether a lost session to `id` should be redialled.
    fn wants(&self, id: &DeviceId) -> bool {
        (self.present || self.is_busy(id)) && self.phone.desktops().iter().any(|peer| peer.id == *id)
    }

    async fn relay(&self, event: SessionEvent) {
        if let SessionEvent::Received { from, share } = event {
            self.emit(ClientEvent::Received { from, share }).await;
        }
    }

    fn desktops(&self) -> Vec<DesktopState> {
        let live = |id: &DeviceId| self.sessions.get(id).is_some_and(SessionHandle::is_live);
        self.phone
            .desktops()
            .iter()
            .map(|peer| DesktopState { peer: peer.clone(), connected: live(&peer.id) })
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
