//! The phone's actor over [`Phone`]: its live sessions, presence with reconnection, and the events an app shows.
//! Shared by the headless phone and the Android bindings.

use std::collections::HashMap;
use std::net::SocketAddr;
use std::time::Duration;

use link_proto::CloseCode;
use link_proto::message::{Message, Share};
use link_proto::session::Role;
use tokio::sync::{mpsc, oneshot};
use tokio::task::JoinSet;
use tokio::time::Instant;

use crate::identity::DeviceId;
use crate::phone::{self, PairTarget, Phone};
use crate::reach::Via;
use crate::session::{self, SessionEvent, SessionHandle};
use crate::store::{Feature, Peer};
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
}

enum Command {
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
    /// The live sessions of desktops whose switch for `feature` is on.
    Sessions {
        feature: Feature,
        reply: oneshot::Sender<Vec<SessionHandle>>,
    },
}

/// The handle apps hold; every method is answered by the [`ClientActor`].
#[derive(Clone)]
pub struct Client(mpsc::Sender<Command>);

pub struct ClientActor {
    phone: Phone,
    commands: mpsc::Receiver<Command>,
    events: mpsc::Sender<ClientEvent>,
    sessions: HashMap<DeviceId, SessionHandle>,
    running: JoinSet<Ended>,
    session_events_tx: mpsc::Sender<SessionEvent>,
    session_events: mpsc::Receiver<SessionEvent>,
    present: bool,
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

/// The caller runs the actor in a task it owns and reads the events.
pub fn client(phone: Phone) -> (Client, ClientActor, mpsc::Receiver<ClientEvent>) {
    let (commands_tx, commands) = mpsc::channel(16);
    let (events, events_rx) = mpsc::channel(64);
    let (session_events_tx, session_events) = mpsc::channel(16);
    let actor = ClientActor {
        phone,
        commands,
        events,
        sessions: HashMap::new(),
        running: JoinSet::new(),
        session_events_tx,
        session_events,
        present: false,
        retries: HashMap::new(),
    };
    (Client(commands_tx), actor, events_rx)
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
        self.0.send(Command::SetPresent { present }).await.map_err(|_| Error::Stopped)
    }

    /// Sends `share`, connecting first if needed, and waits for the desktop's ack.
    pub async fn share(&self, id: DeviceId, share: Share) -> Result<(), Error> {
        share.check()?;
        self.session(id).await?.share(share).await
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

    async fn session(&self, id: DeviceId) -> Result<SessionHandle, Error> {
        self.request(|reply| Command::Session { id, reply }).await?
    }

    async fn request<T>(&self, make: impl FnOnce(oneshot::Sender<T>) -> Command) -> Result<T, Error> {
        let (reply, response) = oneshot::channel();
        self.0.send(make(reply)).await.map_err(|_| Error::Stopped)?;
        response.await.map_err(|_| Error::Stopped)
    }
}

impl ClientActor {
    /// Runs until every [`Client`] is dropped, then closes the sessions and waits for the closes to be sent.
    pub async fn run(mut self) {
        loop {
            let next_retry = self.retries.values().map(|retry| retry.at).min();
            tokio::select! {
                command = self.commands.recv() => match command {
                    Some(command) => self.handle(command).await,
                    None => break,
                },
                Some(joined) = self.running.join_next() => match joined {
                    Ok(ended) => self.ended(ended).await,
                    Err(join) => log::warn!("session task: {join}"),
                },
                Some(event) = self.session_events.recv() => self.relay(event).await,
                () = sleep_until(next_retry) => self.retry_due().await,
            }
        }
        for handle in self.sessions.values() {
            handle.close(CloseCode::Done);
        }
        self.phone.finish().await;
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
            Command::SetSharing { id, feature, on, reply } => {
                drop(reply.send(self.phone.set_sharing(&id, feature, on)));
            }
            Command::Sessions { feature, reply } => {
                let sessions = self
                    .phone
                    .desktops()
                    .iter()
                    .filter(|peer| peer.sharing.allows(feature))
                    .filter_map(|peer| self.sessions.get(&peer.id).filter(|handle| handle.is_live()).cloned())
                    .collect();
                drop(reply.send(sessions));
            }
        }
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
        let events = self.session_events_tx.clone();
        let (handle, actor) = session::session(connection, control, Role::Phone, id.clone(), events);
        let (task_id, stable_id) = (id.clone(), handle.stable_id());
        self.running.spawn(async move { Ended { id: task_id, stable_id, result: actor.run().await } });
        if let Some(old) = self.sessions.insert(id.clone(), handle.clone()) {
            old.close(CloseCode::Done);
        }
        self.retries.remove(&id);
        log::info!("{id}: connected at {addr}");
        self.emit(ClientEvent::Connected { desktop, addr, via, resumed }).await;
        handle
    }

    async fn ended(&mut self, ended: Ended) {
        let Ended { id, stable_id, result } = ended;
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
        self.phone.set_present(present);
        if !present {
            self.retries.clear();
            for handle in self.sessions.values() {
                handle.close(CloseCode::Done);
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

    /// Whether a lost session to `id` should be redialled.
    fn wants(&self, id: &DeviceId) -> bool {
        self.present && self.phone.desktops().iter().any(|peer| peer.id == *id)
    }

    async fn relay(&self, event: SessionEvent) {
        match event {
            SessionEvent::Received { from, share } => self.emit(ClientEvent::Received { from, share }).await,
            SessionEvent::Message { from, message } => {
                let allowed = feature_of(&message).is_some_and(|feature| {
                    self.phone.desktops().iter().any(|peer| peer.id == from && peer.sharing.allows(feature))
                });
                if allowed {
                    self.emit(ClientEvent::Message { from, message }).await;
                } else {
                    log::info!("{from}: dropped a {} its switch does not allow", message.kind());
                }
            }
            SessionEvent::Unpaired { .. } => {}
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

/// The per-desktop switch that governs a feature message.
fn feature_of(message: &Message) -> Option<Feature> {
    match message {
        Message::NotificationPosted(_)
        | Message::NotificationRemoved(_)
        | Message::NotificationAction(_)
        | Message::NotificationDismiss(_) => Some(Feature::Notifications),
        Message::MediaPlayer(_) | Message::MediaGone(_) | Message::MediaCommand(_) => Some(Feature::Media),
        Message::Ring(_) | Message::Ringing(_) => Some(Feature::Ring),
        Message::Call(_) | Message::CallAction(_) => Some(Feature::Calls),
        _ => None,
    }
}

async fn sleep_until(deadline: Option<Instant>) {
    match deadline {
        Some(deadline) => tokio::time::sleep_until(deadline).await,
        None => std::future::pending().await,
    }
}
