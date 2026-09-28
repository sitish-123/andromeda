//! LocalSend v2 as a second transfer backend (`link/ARCHITECTURE.md`, LocalSend). The actor owns the listeners, the
//! nearby peers, and the one incoming session; connections and sends run as its tasks and report back to it. What it
//! reports goes to the hub as the same signals a Link transfer makes.

mod cert;
mod client;
mod http;
mod server;
mod tls;
mod wire;

use std::collections::{BTreeMap, HashMap};
use std::fmt::Write as _;
use std::fs::File;
use std::net::{IpAddr, Ipv4Addr, SocketAddr};
use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::time::Duration;

use anyhow::Context;
use link_core::inbox::{self, Inbox};
use link_core::proto::message::TransferId;
use link_core::transfer::{CONSENT_TIMEOUT, Source, Status};
use ring::rand::{SecureRandom, SystemRandom};
use tokio::net::{TcpListener, UdpSocket};
use tokio::sync::{mpsc, oneshot, watch};
use tokio::task::{AbortHandle, JoinSet};
use tokio::time::Instant;

use crate::hub::Signal;
use wire::{Announcement, FileInfo, Info, PrepareUpload, Prepared};

pub const PREFIX: &str = "localsend:";

/// `(localsend:<fingerprint>, alias)` of each peer on the LAN.
pub type Nearby = watch::Receiver<Vec<(String, String)>>;
const PROGRESS_EVERY: Duration = Duration::from_millis(250);

/// What a connection's `prepare-upload` gets.
enum Answer {
    Accepted(Prepared),
    Status(u16),
}

/// What an accepted upload writes into.
struct Ticket {
    part: PathBuf,
    size: u64,
}

enum Received {
    Complete {
        digest: Vec<u8>,
    },
    /// Longer or shorter than the file's size.
    WrongSize,
    Failed,
}

enum Command {
    SetVisible { visible: bool },
    Prepare { request: PrepareUpload, reply: oneshot::Sender<Answer> },
    Upload { session: String, file: String, token: String, reply: oneshot::Sender<Result<Ticket, u16>> },
    Uploaded { session: String, file: String, received: Received, reply: oneshot::Sender<u16> },
    Progress { id: TransferId, bytes: u64 },
    SessionProgress { session: String, bytes: u64 },
    CancelSession { session: String },
    Announced { announcement: Announcement, from: IpAddr },
    Decide { id: TransferId, accept: bool, reply: oneshot::Sender<bool> },
    Cancel { id: TransferId, reply: oneshot::Sender<bool> },
    Send { fingerprint: String, sources: Vec<Source>, reply: oneshot::Sender<Result<TransferId, String>> },
    SendEnded { id: TransferId, status: Status },
}

#[derive(Clone)]
pub struct LocalSendHandle(mpsc::Sender<Command>);

struct InFile {
    token: String,
    name: String,
    size: u64,
    sha256: Option<String>,
    part: PathBuf,
    streaming: bool,
    done: Option<bool>,
    path: Option<PathBuf>,
}

struct Session {
    id: TransferId,
    /// The LocalSend session id.
    key: String,
    files: BTreeMap<String, InFile>,
    pending: Option<(oneshot::Sender<Answer>, Instant)>,
    last_progress: Option<Instant>,
}

pub struct LocalSend {
    info: Info,
    server: Arc<rustls::ServerConfig>,
    inbox: Inbox,
    handle: LocalSendHandle,
    commands: mpsc::Receiver<Command>,
    signals: mpsc::UnboundedSender<Signal>,
    nearby: watch::Sender<Vec<(String, String)>>,
    peers: HashMap<String, (Info, IpAddr)>,
    listening: JoinSet<()>,
    sending: JoinSet<()>,
    /// Sends in flight: the task, the total bytes, and when progress was last reported.
    outgoing: HashMap<TransferId, (AbortHandle, u64, Option<Instant>)>,
    udp: Option<Arc<UdpSocket>>,
    session: Option<Session>,
}

impl LocalSend {
    pub fn new(
        state: &Path,
        downloads: PathBuf,
        alias: String,
        signals: mpsc::UnboundedSender<Signal>,
    ) -> anyhow::Result<(Self, LocalSendHandle, Nearby)> {
        let identity = cert::Identity::load_or_create(state).context("the LocalSend certificate")?;
        let info = Info {
            alias,
            version: wire::VERSION.to_owned(),
            device_model: Some("Linux".to_owned()),
            device_type: Some("desktop".to_owned()),
            fingerprint: identity.fingerprint(),
            port: wire::PORT,
            protocol: "https".to_owned(),
            download: false,
        };
        let (commands_tx, commands) = mpsc::channel(32);
        let handle = LocalSendHandle(commands_tx);
        let (nearby, nearby_rx) = watch::channel(Vec::new());
        let actor = Self {
            info,
            server: tls::server(&identity)?,
            inbox: Inbox::new(downloads, state)?,
            handle: handle.clone(),
            commands,
            signals,
            nearby,
            peers: HashMap::new(),
            listening: JoinSet::new(),
            sending: JoinSet::new(),
            outgoing: HashMap::new(),
            udp: None,
            session: None,
        };
        Ok((actor, handle, nearby_rx))
    }

    pub async fn run(mut self) {
        loop {
            let deadline = self.session.as_ref().and_then(|session| session.pending.as_ref()).map(|(_, at)| *at);
            tokio::select! {
                command = self.commands.recv() => match command {
                    Some(command) => self.handle(command).await,
                    None => return,
                },
                Some(_) = self.listening.join_next() => {}
                Some(_) = self.sending.join_next() => {}
                () = sleep_until(deadline) => self.answer(false),
            }
        }
    }

    async fn handle(&mut self, command: Command) {
        match command {
            Command::SetVisible { visible } => self.set_visible(visible).await,
            Command::Prepare { request, reply } => self.prepare(request, reply),
            Command::Upload { session, file, token, reply } => drop(reply.send(self.upload(&session, &file, &token))),
            Command::Uploaded { session, file, received, reply } => {
                let _ = reply.send(self.uploaded(&session, &file, received));
            }
            Command::Progress { id, bytes } => self.progress(id, bytes),
            Command::SessionProgress { session, bytes } => {
                if let Some(id) = self.session.as_ref().filter(|open| open.key == session).map(|open| open.id) {
                    self.progress(id, bytes);
                }
            }
            Command::CancelSession { session } => {
                if self.session.as_ref().is_some_and(|open| open.key == session) {
                    self.finish_session(Status::Cancelled);
                }
            }
            Command::Announced { announcement, from } => self.announced(announcement, from),
            Command::Decide { id, accept, reply } => {
                let waiting =
                    self.session.as_ref().is_some_and(|session| session.id == id && session.pending.is_some());
                if waiting {
                    self.answer(accept);
                }
                let _ = reply.send(waiting);
            }
            Command::Cancel { id, reply } => drop(reply.send(self.cancel(id))),
            Command::Send { fingerprint, sources, reply } => drop(reply.send(self.send(&fingerprint, sources))),
            Command::SendEnded { id, status } => {
                if self.outgoing.remove(&id).is_some() {
                    self.signal(Signal::Finished { id, status, paths: Vec::new() });
                }
            }
        }
    }

    async fn set_visible(&mut self, visible: bool) {
        self.listening.abort_all();
        self.udp = None;
        self.peers.clear();
        self.publish_nearby();
        if !visible {
            log::info!("LocalSend: hidden");
            return;
        }
        if let Err(error) = self.listen().await {
            log::error!("LocalSend: {error:#}");
            self.listening.abort_all();
            self.udp = None;
        }
    }

    async fn listen(&mut self) -> anyhow::Result<()> {
        let any = SocketAddr::from((Ipv4Addr::UNSPECIFIED, wire::PORT));
        let listener = TcpListener::bind(any).await.context("binding TCP 53317")?;
        let udp = Arc::new(UdpSocket::bind(any).await.context("binding UDP 53317")?);
        udp.join_multicast_v4(Ipv4Addr::from(wire::MULTICAST), Ipv4Addr::UNSPECIFIED)?;
        let info = Arc::new(serde_json::to_vec(&self.info)?);
        let acceptor = tokio_rustls::TlsAcceptor::from(self.server.clone());
        self.listening.spawn(server::accept(listener, acceptor, self.handle.clone(), info));
        self.listening.spawn(listen_udp(udp.clone(), self.handle.clone()));
        let announcement = Announcement { info: self.info.clone(), announce: true };
        udp.send_to(&serde_json::to_vec(&announcement)?, (Ipv4Addr::from(wire::MULTICAST), wire::PORT)).await?;
        self.udp = Some(udp);
        log::info!("LocalSend: visible as {:?}, fingerprint {}", self.info.alias, self.info.fingerprint);
        Ok(())
    }

    fn announced(&mut self, announcement: Announcement, from: IpAddr) {
        let Announcement { info, announce } = announcement;
        if info.fingerprint == self.info.fingerprint || info.fingerprint.is_empty() || info.protocol != "https" {
            return;
        }
        log::info!("LocalSend: {:?} at {from}", info.alias);
        if announce {
            let (own, peer, udp) = (self.info.clone(), info.clone(), self.udp.clone());
            self.listening.spawn(async move {
                if let Err(error) = client::register(&own, &peer, from).await {
                    log::info!("LocalSend: registering with {:?}: {error:#}; answering by multicast", peer.alias);
                    reply_by_multicast(udp, own).await;
                }
            });
        }
        self.peers.insert(info.fingerprint.clone(), (info, from));
        self.publish_nearby();
    }

    fn publish_nearby(&self) {
        let mut nearby: Vec<(String, String)> = self
            .peers
            .values()
            .map(|(info, _)| (format!("{PREFIX}{}", info.fingerprint), info.alias.clone()))
            .collect();
        nearby.sort();
        self.nearby.send_if_modified(|current| {
            let changed = *current != nearby;
            *current = nearby;
            changed
        });
    }

    fn prepare(&mut self, request: PrepareUpload, reply: oneshot::Sender<Answer>) {
        if self.session.is_some() {
            drop(reply.send(Answer::Status(409)));
            return;
        }
        let PrepareUpload { info, files } = request;
        if files.is_empty() || files.len() > 1000 {
            drop(reply.send(Answer::Status(400)));
            return;
        }
        let id = random_id();
        let total = files.values().map(|file| file.size).fold(0, u64::saturating_add);
        let device = format!("{PREFIX}{}", info.fingerprint);
        let space = self.inbox.space().map_or(0, |space| space.free);
        if total > space {
            log::info!("LocalSend: {:?} offers {total} bytes, more than the free space", info.alias);
            drop(reply.send(Answer::Status(403)));
            self.signal(Signal::Finished { id, status: Status::NoSpace, paths: Vec::new() });
            return;
        }
        let files: BTreeMap<String, InFile> = files.into_iter().map(|(key, file)| (key, in_file(file))).collect();
        let offered = files.values().map(|file| (file.name.clone(), file.size)).collect();
        log::info!("LocalSend: {:?} offers {} files", info.alias, files.len());
        let deadline = Instant::now() + CONSENT_TIMEOUT;
        self.session = Some(Session { id, key: token(), files, pending: Some((reply, deadline)), last_progress: None });
        self.signal(Signal::Offered { id, device, files: offered });
    }

    /// Answers the waiting `prepare-upload`: tokens and part files when accepted, 403 otherwise.
    fn answer(&mut self, accept: bool) {
        let Some(session) = self.session.as_mut() else { return };
        let Some((reply, _)) = session.pending.take() else { return };
        if !accept {
            drop(reply.send(Answer::Status(403)));
            self.finish_session(Status::Declined);
            return;
        }
        for file in session.files.values_mut() {
            match self.inbox.create_part(&file.name) {
                Ok(part) => file.part = part,
                Err(error) => {
                    log::error!("LocalSend: {error}");
                    drop(reply.send(Answer::Status(500)));
                    self.finish_session(Status::Failed);
                    return;
                }
            }
        }
        let files = session.files.iter().map(|(key, file)| (key.clone(), file.token.clone())).collect();
        drop(reply.send(Answer::Accepted(Prepared { session_id: session.key.clone(), files })));
    }

    fn upload(&mut self, session: &str, file: &str, token: &str) -> Result<Ticket, u16> {
        let open = self.session.as_mut().filter(|open| open.key == session && open.pending.is_none()).ok_or(403_u16)?;
        let entry = open.files.get_mut(file).filter(|entry| entry.token == token).ok_or(403_u16)?;
        if entry.streaming || entry.done.is_some() {
            return Err(409);
        }
        entry.streaming = true;
        Ok(Ticket { part: entry.part.clone(), size: entry.size })
    }

    fn uploaded(&mut self, session: &str, file: &str, received: Received) -> u16 {
        let Some(open) = self.session.as_mut().filter(|open| open.key == session) else { return 403 };
        let Some(entry) = open.files.get_mut(file) else { return 403 };
        entry.streaming = false;
        let (ok, status) = match received {
            Received::Complete { digest } => {
                let hex = hex(&digest);
                if entry.sha256.as_ref().is_some_and(|sha| !sha.eq_ignore_ascii_case(&hex)) {
                    log::warn!("LocalSend: {} does not match its hash", entry.name);
                    (false, 422)
                } else {
                    match self.inbox.publish(&entry.part, &entry.name) {
                        Ok(path) => {
                            log::info!("LocalSend: received {}", path.display());
                            entry.path = Some(path);
                            (true, 200)
                        }
                        Err(error) => {
                            log::error!("LocalSend: publishing {}: {error}", entry.name);
                            (false, 500)
                        }
                    }
                }
            }
            Received::WrongSize => (false, 400),
            Received::Failed => (false, 500),
        };
        if !ok {
            inbox::remove_quietly(&entry.part);
        }
        entry.done = Some(ok);
        let finished = open.files.values().all(|file| file.done.is_some());
        let all_ok = open.files.values().all(|file| file.done == Some(true));
        if finished {
            self.finish_session(if all_ok { Status::Done } else { Status::Failed });
        }
        status
    }

    fn progress(&mut self, id: TransferId, bytes: u64) {
        if let Some((_, total, last)) = self.outgoing.get_mut(&id) {
            if last.is_some_and(|last| last.elapsed() < PROGRESS_EVERY) {
                return;
            }
            *last = Some(Instant::now());
            let total = *total;
            self.signal(Signal::Progress { id, bytes, total });
            return;
        }
        let Some(open) = self.session.as_mut().filter(|open| open.id == id) else { return };
        if open.last_progress.is_some_and(|last| last.elapsed() < PROGRESS_EVERY) {
            return;
        }
        open.last_progress = Some(Instant::now());
        let total = open.files.values().map(|file| file.size).sum();
        let done: u64 = open.files.values().filter(|file| file.done.is_some()).map(|file| file.size).sum();
        self.signal(Signal::Progress { id, bytes: (done + bytes).min(total), total });
    }

    fn cancel(&mut self, id: TransferId) -> bool {
        if let Some((task, _, _)) = self.outgoing.remove(&id) {
            task.abort();
            self.signal(Signal::Finished { id, status: Status::Cancelled, paths: Vec::new() });
            return true;
        }
        if self.session.as_ref().is_some_and(|session| session.id == id) {
            if let Some((reply, _)) = self.session.as_mut().and_then(|session| session.pending.take()) {
                drop(reply.send(Answer::Status(403)));
            }
            self.finish_session(Status::Cancelled);
            return true;
        }
        false
    }

    fn finish_session(&mut self, status: Status) {
        let Some(session) = self.session.take() else { return };
        for file in session.files.values().filter(|file| file.path.is_none() && !file.part.as_os_str().is_empty()) {
            inbox::remove_quietly(&file.part);
        }
        let paths = session.files.into_values().filter_map(|file| file.path).collect();
        log::info!("LocalSend: session {} ended {}", session.key, status.as_str());
        self.signal(Signal::Finished { id: session.id, status, paths });
    }

    fn send(&mut self, fingerprint: &str, sources: Vec<Source>) -> Result<TransferId, String> {
        let (peer, address) =
            self.peers.get(fingerprint).cloned().ok_or_else(|| format!("{PREFIX}{fingerprint} is not nearby"))?;
        let id = random_id();
        let files: Vec<(File, u64, String, String)> = sources.into_iter().map(Source::into_parts).collect();
        let total = files.iter().map(|file| file.1).sum();
        let (own, handle) = (self.info.clone(), self.handle.clone());
        let task = self.sending.spawn(async move {
            let status = client::send(&own, &peer, address, files, |bytes| handle.progress(id, bytes)).await;
            handle.send_ended(id, status).await;
        });
        self.outgoing.insert(id, (task, total, None));
        Ok(id)
    }

    fn signal(&self, signal: Signal) {
        if self.signals.send(signal).is_err() {
            log::debug!("nobody receives LocalSend signals");
        }
    }
}

fn in_file(file: FileInfo) -> InFile {
    InFile {
        token: token(),
        name: inbox::sanitize(&file.file_name),
        size: file.size,
        sha256: file.sha256.filter(|sha| !sha.is_empty()),
        part: PathBuf::new(),
        streaming: false,
        done: None,
        path: None,
    }
}

/// Lowercase hex, as LocalSend writes fingerprints and hashes.
pub fn hex(bytes: &[u8]) -> String {
    bytes.iter().fold(String::with_capacity(2 * bytes.len()), |mut out, byte| {
        let _ = write!(out, "{byte:02x}");
        out
    })
}

fn random_id() -> TransferId {
    let mut id = TransferId([0; 16]);
    if SystemRandom::new().fill(&mut id.0).is_err() {
        log::error!("system randomness unavailable");
    }
    id
}

/// A session id or file token: 16 random bytes in hex.
fn token() -> String {
    random_id().to_hex()
}

async fn listen_udp(socket: Arc<UdpSocket>, handle: LocalSendHandle) {
    let mut buf = vec![0; 64 * 1024];
    while let Ok((len, from)) = socket.recv_from(&mut buf).await {
        match serde_json::from_slice::<Announcement>(&buf[..len]) {
            Ok(announcement) => handle.announced(announcement, from.ip()).await,
            Err(error) => log::debug!("LocalSend: a datagram from {from} is not an announcement: {error}"),
        }
    }
}

async fn reply_by_multicast(udp: Option<Arc<UdpSocket>>, own: Info) {
    let Some(udp) = udp else { return };
    let Ok(bytes) = serde_json::to_vec(&Announcement { info: own, announce: false }) else { return };
    if let Err(error) = udp.send_to(&bytes, (Ipv4Addr::from(wire::MULTICAST), wire::PORT)).await {
        log::info!("LocalSend: multicast reply: {error}");
    }
}

async fn sleep_until(deadline: Option<Instant>) {
    match deadline {
        Some(deadline) => tokio::time::sleep_until(deadline).await,
        None => std::future::pending().await,
    }
}

impl LocalSendHandle {
    async fn tell(&self, command: Command) {
        if self.0.send(command).await.is_err() {
            log::debug!("the LocalSend actor stopped");
        }
    }

    async fn request<T>(&self, make: impl FnOnce(oneshot::Sender<T>) -> Command) -> Option<T> {
        let (reply, response) = oneshot::channel();
        self.0.send(make(reply)).await.ok()?;
        response.await.ok()
    }

    pub async fn set_visible(&self, visible: bool) {
        self.tell(Command::SetVisible { visible }).await;
    }

    pub async fn decide(&self, id: TransferId, accept: bool) -> bool {
        self.request(|reply| Command::Decide { id, accept, reply }).await.unwrap_or(false)
    }

    pub async fn cancel(&self, id: TransferId) -> bool {
        self.request(|reply| Command::Cancel { id, reply }).await.unwrap_or(false)
    }

    /// Sends to the nearby peer with `fingerprint`; the transfer signals report the rest.
    pub async fn send(&self, fingerprint: String, sources: Vec<Source>) -> Result<TransferId, String> {
        self.request(|reply| Command::Send { fingerprint, sources, reply })
            .await
            .unwrap_or_else(|| Err("LocalSend stopped".to_owned()))
    }

    async fn prepare(&self, request: PrepareUpload) -> Answer {
        self.request(|reply| Command::Prepare { request, reply }).await.unwrap_or(Answer::Status(500))
    }

    async fn upload(&self, session: String, file: String, token: String) -> Result<Ticket, u16> {
        self.request(|reply| Command::Upload { session, file, token, reply }).await.unwrap_or(Err(500))
    }

    async fn uploaded(&self, session: String, file: String, received: Received) -> u16 {
        self.request(|reply| Command::Uploaded { session, file, received, reply }).await.unwrap_or(500)
    }

    fn progress(&self, id: TransferId, bytes: u64) {
        drop(self.0.try_send(Command::Progress { id, bytes }));
    }

    fn session_progress(&self, session: &str, bytes: u64) {
        drop(self.0.try_send(Command::SessionProgress { session: session.to_owned(), bytes }));
    }

    async fn cancel_session(&self, session: String) {
        self.tell(Command::CancelSession { session }).await;
    }

    async fn announced(&self, announcement: Announcement, from: IpAddr) {
        self.tell(Command::Announced { announcement, from }).await;
    }

    async fn send_ended(&self, id: TransferId, status: Status) {
        self.tell(Command::SendEnded { id, status }).await;
    }
}
