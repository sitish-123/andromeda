//! Quick Share as a backend of the daemon: findable only while the user makes it visible, and every offer accepted or
//! declined by the user through the shell. D-Bus `org.umbriel.Link1.QuickShare` on the Link object.

use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::time::Duration;

use link_quickshare::advertise::{Ble, Mdns};
use link_quickshare::endpoint::{DEVICE_LAPTOP, Endpoint};
use link_quickshare::receive::{self, Offer, Outcome, TextKind};
use tokio::net::TcpListener;
use tokio::sync::{mpsc, oneshot, watch};
use tokio::task::JoinSet;
use zbus::fdo;
use zbus::object_server::SignalEmitter;

use crate::dbus::PATH;

/// Unassigned at IANA, next to Link's 4717/udp; the ufw profile `Umbriel Link` opens it.
const PORT: u16 = 4718;
/// Android gives up on an unanswered offer after about a minute.
const CONSENT_TIMEOUT: Duration = Duration::from_secs(60);
const ADAPTER: &str = "/org/bluez/hci0";

pub enum Event {
    Offer { id: u64, offer: Offer },
    Finished { id: u64, status: &'static str, files: Vec<PathBuf>, texts: Vec<(TextKind, String)>, error: String },
}

enum Command {
    SetVisible { visible: bool, reply: oneshot::Sender<Result<(), String>> },
    Decide { id: u64, accept: bool },
}

#[derive(Clone)]
pub struct Handle(mpsc::Sender<Command>);

struct Visible {
    listener: TcpListener,
    _mdns: Mdns,
    _ble: Option<Ble>,
}

pub struct QuickShare {
    own: Endpoint,
    downloads: PathBuf,
    marker: PathBuf,
    visible: Option<Visible>,
    visible_tx: watch::Sender<bool>,
    commands: mpsc::Receiver<Command>,
    offers_tx: mpsc::Sender<(u64, Offer, oneshot::Sender<bool>)>,
    offers: mpsc::Receiver<(u64, Offer, oneshot::Sender<bool>)>,
    pending: HashMap<u64, oneshot::Sender<bool>>,
    connections: JoinSet<(u64, link_quickshare::Result<Outcome>)>,
    events: mpsc::Sender<Event>,
    next_id: u64,
}

impl QuickShare {
    /// Visibility persists across restarts as a marker file in the state directory.
    pub fn new(
        name: &str,
        state_dir: &Path,
    ) -> anyhow::Result<(Self, Handle, watch::Receiver<bool>, mpsc::Receiver<Event>)> {
        let (commands_tx, commands) = mpsc::channel(8);
        let (offers_tx, offers) = mpsc::channel(8);
        let (events, events_rx) = mpsc::channel(16);
        let (visible_tx, visible_rx) = watch::channel(false);
        let this = Self {
            own: Endpoint::new(name, DEVICE_LAPTOP)?,
            downloads: downloads_dir(),
            marker: state_dir.join("quickshare-visible"),
            visible: None,
            visible_tx,
            commands,
            offers_tx,
            offers,
            pending: HashMap::new(),
            connections: JoinSet::new(),
            events,
            next_id: 1,
        };
        Ok((this, Handle(commands_tx), visible_rx, events_rx))
    }

    pub async fn run(mut self) -> anyhow::Result<()> {
        if self.marker.exists()
            && let Err(error) = self.set_visible(true).await
        {
            log::warn!("quick share: {error}");
        }
        loop {
            tokio::select! {
                command = self.commands.recv() => match command {
                    Some(Command::SetVisible { visible, reply }) => drop(reply.send(self.set_visible(visible).await)),
                    Some(Command::Decide { id, accept }) => {
                        if let Some(decide) = self.pending.remove(&id) {
                            // The connection may have ended while the user decided.
                            let _ = decide.send(accept);
                        }
                    }
                    None => return Ok(()),
                },
                accepted = accept(self.visible.as_ref()) => match accepted {
                    Ok((stream, remote)) => self.spawn(stream, remote),
                    Err(error) => log::warn!("quick share accept: {error}"),
                },
                Some((id, offer, decide)) = self.offers.recv() => {
                    self.pending.insert(id, decide);
                    self.emit(Event::Offer { id, offer }).await;
                }
                Some(joined) = self.connections.join_next() => match joined {
                    Ok((id, outcome)) => self.finished(id, outcome).await,
                    Err(error) => log::error!("quick share connection task: {error}"),
                },
            }
        }
    }

    async fn set_visible(&mut self, visible: bool) -> Result<(), String> {
        if visible == self.visible.is_some() {
            return Ok(());
        }
        if visible {
            let listener = match TcpListener::bind(("::", PORT)).await {
                Ok(listener) => listener,
                Err(error) => {
                    log::warn!("quick share port {PORT}: {error}; choosing another");
                    TcpListener::bind(("::", 0)).await.map_err(|e| format!("listening: {e}"))?
                }
            };
            let port = listener.local_addr().map_err(|e| e.to_string())?.port();
            let mdns = Mdns::advertise(&self.own, port).map_err(|e| format!("mDNS: {e}"))?;
            // Without the BLE hint Android phones do not look for us, but ones already scanning still find us.
            let ble = Ble::advertise(ADAPTER).await.inspect_err(|e| log::warn!("quick share BLE hint: {e}")).ok();
            log::info!("quick share visible on port {port} as {:?}", self.own.name);
            self.visible = Some(Visible { listener, _mdns: mdns, _ble: ble });
            std::fs::write(&self.marker, b"").map_err(|e| e.to_string())?;
        } else {
            self.visible = None;
            log::info!("quick share hidden");
            if let Err(error) = std::fs::remove_file(&self.marker) {
                log::debug!("quick share marker: {error}");
            }
        }
        self.visible_tx.send_replace(visible);
        Ok(())
    }

    fn spawn(&mut self, stream: tokio::net::TcpStream, remote: std::net::SocketAddr) {
        let id = self.next_id;
        self.next_id += 1;
        log::info!("quick share: connection {id} from {remote}");
        let offers = self.offers_tx.clone();
        let dir = self.downloads.clone();
        self.connections.spawn(async move {
            let consent = move |offer: Offer| async move {
                let (decide, decided) = oneshot::channel();
                if offers.send((id, offer, decide)).await.is_err() {
                    return false;
                }
                tokio::time::timeout(CONSENT_TIMEOUT, decided).await.ok().and_then(Result::ok).unwrap_or(false)
            };
            (id, receive::receive(stream, &dir, consent).await)
        });
    }

    async fn finished(&mut self, id: u64, outcome: link_quickshare::Result<Outcome>) {
        self.pending.remove(&id);
        let event = match outcome {
            Ok(Outcome::Received { files, texts }) => {
                Event::Finished { id, status: "received", files, texts, error: String::new() }
            }
            Ok(Outcome::Declined) => empty(id, "declined", String::new()),
            Ok(Outcome::Cancelled) => empty(id, "cancelled", String::new()),
            Err(error) => {
                log::info!("quick share: connection {id}: {error}");
                empty(id, "failed", error.to_string())
            }
        };
        self.emit(event).await;
    }

    async fn emit(&self, event: Event) {
        if self.events.send(event).await.is_err() {
            log::debug!("no D-Bus forwarder for quick share events");
        }
    }
}

fn empty(id: u64, status: &'static str, error: String) -> Event {
    Event::Finished { id, status, files: Vec::new(), texts: Vec::new(), error }
}

async fn accept(visible: Option<&Visible>) -> std::io::Result<(tokio::net::TcpStream, std::net::SocketAddr)> {
    match visible {
        Some(visible) => visible.listener.accept().await,
        None => std::future::pending().await,
    }
}

/// `XDG_DOWNLOAD_DIR` from user-dirs.dirs, else ~/Downloads.
fn downloads_dir() -> PathBuf {
    let home = std::env::var_os("HOME").map_or_else(|| PathBuf::from("/tmp"), PathBuf::from);
    let config = std::env::var_os("XDG_CONFIG_HOME").map_or_else(|| home.join(".config"), PathBuf::from);
    let configured = std::fs::read_to_string(config.join("user-dirs.dirs")).ok().and_then(|dirs| {
        dirs.lines().find_map(|line| {
            let value = line.strip_prefix("XDG_DOWNLOAD_DIR=")?.trim().trim_matches('"');
            Some(PathBuf::from(value.replace("$HOME", &home.to_string_lossy())))
        })
    });
    configured.unwrap_or_else(|| home.join("Downloads"))
}

impl Handle {
    async fn set_visible(&self, visible: bool) -> Result<(), String> {
        let (reply, result) = oneshot::channel();
        self.0.send(Command::SetVisible { visible, reply }).await.map_err(|_| "stopped".to_owned())?;
        result.await.map_err(|_| "stopped".to_owned())?
    }

    async fn decide(&self, id: u64, accept: bool) {
        if self.0.send(Command::Decide { id, accept }).await.is_err() {
            log::debug!("quick share stopped");
        }
    }
}

struct Interface {
    handle: Handle,
    visible: watch::Receiver<bool>,
    name: String,
}

#[zbus::interface(name = "org.umbriel.Link1.QuickShare")]
impl Interface {
    async fn accept(&self, id: u64) {
        self.handle.decide(id, true).await;
    }

    async fn decline(&self, id: u64) {
        self.handle.decide(id, false).await;
    }

    #[zbus(property)]
    fn visible(&self) -> bool {
        *self.visible.borrow()
    }

    #[zbus(property)]
    async fn set_visible(&mut self, visible: bool) -> fdo::Result<()> {
        self.handle.set_visible(visible).await.map_err(fdo::Error::Failed)
    }

    #[zbus(property)]
    fn name(&self) -> String {
        self.name.clone()
    }

    /// `files` are (name, size); `texts` are the kinds of shared text: text, url, address, phone.
    #[zbus(signal)]
    async fn offer(
        emitter: &SignalEmitter<'_>,
        id: u64,
        sender: &str,
        pin: &str,
        files: Vec<(String, i64)>,
        texts: Vec<String>,
    ) -> zbus::Result<()>;

    /// `status` is received, declined, cancelled, or failed; `texts` are (kind, text).
    #[zbus(signal)]
    async fn finished(
        emitter: &SignalEmitter<'_>,
        id: u64,
        status: &str,
        files: Vec<String>,
        texts: Vec<(String, String)>,
        error: &str,
    ) -> zbus::Result<()>;
}

pub async fn serve(
    bus: &zbus::Connection,
    handle: Handle,
    visible: watch::Receiver<bool>,
    name: String,
) -> anyhow::Result<()> {
    bus.object_server().at(PATH, Interface { handle, visible, name }).await?;
    Ok(())
}

pub async fn forward(
    bus: &zbus::Connection,
    mut visible: watch::Receiver<bool>,
    mut events: mpsc::Receiver<Event>,
) -> anyhow::Result<()> {
    let iface = bus.object_server().interface::<_, Interface>(PATH).await?;
    let emitter = iface.signal_emitter();
    loop {
        tokio::select! {
            changed = visible.changed() => {
                changed?;
                iface.get().await.visible_changed(emitter).await?;
            }
            Some(event) = events.recv() => match event {
                Event::Offer { id, offer } => {
                    let files = offer.files.iter().map(|f| (f.name.clone(), f.size)).collect();
                    let texts = offer.texts.iter().map(|(kind, _)| kind_str(kind).to_owned()).collect();
                    Interface::offer(emitter, id, &offer.sender, &offer.pin, files, texts).await?;
                }
                Event::Finished { id, status, files, texts, error } => {
                    let files = files.iter().map(|p| p.display().to_string()).collect();
                    let texts = texts.into_iter().map(|(kind, text)| (kind_str(&kind).to_owned(), text)).collect();
                    Interface::finished(emitter, id, status, files, texts, &error).await?;
                }
            },
        }
    }
}

fn kind_str(kind: &TextKind) -> &'static str {
    match kind {
        TextKind::Text => "text",
        TextKind::Url => "url",
        TextKind::Address => "address",
        TextKind::Phone => "phone",
    }
}
