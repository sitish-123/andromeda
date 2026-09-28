//! Kotlin bindings of the phone role for the Android app (`UniFFI`). One `LinkClient` owns a single-worker tokio
//! runtime running `link_core::client`'s actor; every method is a suspend function in Kotlin whose work runs there.

use std::fmt::Write as _;
use std::fs::File;
use std::os::fd::{FromRawFd, OwnedFd};
use std::path::{Path, PathBuf};
use std::sync::Arc;

use link_core::client::{self, Client, ClientEvent};
use link_core::identity::{DeviceId, Identity};
use link_core::inbox::Inbox;
use link_core::phone::{PairTarget, Phone};
use link_core::proto::CloseCode;
use link_core::proto::message::{self, Message, Share, TransferId};
use link_core::proto::pairing::PairingError;
use link_core::store::{self, Peer};
use link_core::transfer::{LocalClip, Source, TransferEvent};
use link_core::uri::PairingUri;
use tokio::sync::{Mutex, mpsc};

uniffi::setup_scaffolding!();

#[derive(Debug, thiserror::Error, uniffi::Error)]
pub enum LinkError {
    #[error("the code does not match")]
    WrongCode,
    #[error("the desktop unpaired this phone")]
    Unpaired,
    #[error("the desktop cannot be reached")]
    Unreachable,
    #[error("{reason}")]
    Rejected { reason: String },
    #[error("{reason}")]
    Failed { reason: String },
}

impl From<link_core::Error> for LinkError {
    fn from(error: link_core::Error) -> Self {
        use link_core::Error;
        match error {
            Error::Pairing(PairingError::Mismatch) | Error::Closed(CloseCode::PairingFailed) => Self::WrongCode,
            Error::Closed(CloseCode::Unpaired) => Self::Unpaired,
            Error::Unreachable | Error::Timeout => Self::Unreachable,
            Error::Share(rejected) => Self::Rejected { reason: rejected.to_string() },
            Error::Decode(invalid) => Self::Rejected { reason: invalid.to_string() },
            other => Self::Failed { reason: other.to_string() },
        }
    }
}

#[derive(Debug, Clone, uniffi::Record)]
pub struct Desktop {
    pub id: String,
    pub name: String,
    pub addresses: Vec<String>,
    /// Unix seconds.
    pub last_seen: u64,
    pub connected: bool,
    /// Connected over Bluetooth, since no IP path answered.
    pub bluetooth: bool,
    pub sharing: Sharing,
}

/// The phone's switches for one desktop.
#[derive(Debug, Clone, Copy, uniffi::Record)]
#[expect(clippy::struct_excessive_bools, reason = "one independent switch per feature")]
pub struct Sharing {
    pub clipboard: bool,
    pub files: bool,
    pub notifications: bool,
    pub media: bool,
    pub ring: bool,
    pub calls: bool,
    /// The desktop may browse this phone's storage; off after pairing.
    pub browse: bool,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, uniffi::Enum)]
pub enum Feature {
    Clipboard,
    Files,
    Notifications,
    Media,
    Ring,
    Calls,
    Browse,
}

#[derive(Debug, Clone, uniffi::Record)]
pub struct NotificationButton {
    pub id: String,
    pub label: String,
    pub reply: bool,
}

/// A phone notification to mirror; the core checks the limits in `link/ARCHITECTURE.md` before sending.
#[derive(Debug, Clone, uniffi::Record)]
pub struct PhoneNotification {
    pub id: String,
    pub app: String,
    pub title: String,
    pub text: String,
    /// PNG.
    pub icon: Option<Vec<u8>>,
    pub actions: Vec<NotificationButton>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, uniffi::Enum)]
pub enum ShareKind {
    Text,
    Link,
}

#[derive(Debug, Clone, uniffi::Enum)]
pub enum LinkEvent {
    Connected {
        desktop_id: String,
    },
    Disconnected {
        desktop_id: String,
    },
    Received {
        desktop_id: String,
        kind: ShareKind,
        text: String,
    },
    /// The desktop unpaired this phone, which has forgotten it.
    Unpaired {
        desktop_id: String,
    },
    /// The desktop offers files; answer with `accept_transfer` or `decline_transfer` within 120 s.
    TransferOffered {
        transfer_id: String,
        desktop_id: String,
        files: Vec<OfferedFile>,
    },
    /// A desktop's clipboard changed. Set `text` at once when present; pull other types with `pull_clip` when an app
    /// reads them.
    ClipOffered {
        desktop_id: String,
        clip_id: u64,
        mimes: Vec<String>,
        size: u64,
        text: Option<String>,
    },
    /// At most every 250 ms per transfer.
    TransferProgress {
        transfer_id: String,
        bytes: u64,
        total: u64,
    },
    /// `status` is done, failed, declined, no-space, too-large, busy, or cancelled; `files` holds what an incoming
    /// transfer published, already verified against its hash.
    TransferFinished {
        transfer_id: String,
        desktop_id: String,
        incoming: bool,
        status: String,
        files: Vec<ReceivedFile>,
    },
    /// Run a mirrored notification's action; `reply_text` only for one that takes `RemoteInput`.
    NotificationAction {
        desktop_id: String,
        id: String,
        action: String,
        reply_text: Option<String>,
    },
    NotificationDismissed {
        desktop_id: String,
        id: String,
    },
    /// A desktop player appeared or changed.
    PlayerChanged {
        desktop_id: String,
        player: MediaPlayer,
    },
    PlayerGone {
        desktop_id: String,
        player: String,
    },
    /// A desktop asks this phone to ring, or to stop.
    RingRequested {
        desktop_id: String,
        on: bool,
    },
    /// The desktop this phone rang started or stopped ringing.
    DesktopRinging {
        desktop_id: String,
        on: bool,
    },
    /// A desktop asks to mute the ringer or decline the ringing call.
    CallActionRequested {
        desktop_id: String,
        action: CallAction,
    },
    /// A desktop commands this phone's player.
    PlayerCommand {
        desktop_id: String,
        player: String,
        command: MediaCommandKind,
        value: Option<u64>,
    },
}

#[derive(Debug, Clone, uniffi::Record)]
pub struct OfferedFile {
    pub name: String,
    pub size: u64,
    pub mime: String,
}

#[derive(Debug, Clone, uniffi::Record)]
pub struct ReceivedFile {
    pub path: String,
    /// Lowercase hex.
    pub sha256: String,
}

/// A file to send: a descriptor the client takes ownership of, which must be a regular file of `size` bytes.
#[derive(Debug, Clone, uniffi::Record)]
pub struct OutgoingFile {
    pub fd: i32,
    pub name: String,
    pub size: u64,
    pub mime: String,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, uniffi::Enum)]
pub enum CallState {
    Ringing,
    Active,
    Idle,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, uniffi::Enum)]
pub enum CallAction {
    Mute,
    Decline,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, uniffi::Enum)]
pub enum PlaybackState {
    Playing,
    Paused,
    Stopped,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, uniffi::Enum)]
pub enum MediaCommandKind {
    Play,
    Pause,
    PlayPause,
    Next,
    Previous,
    /// The value is the absolute position in ms.
    Seek,
    /// The value is 0 to 100.
    Volume,
}

/// A player on either side; the core checks the limits in `link/ARCHITECTURE.md` before sending one.
#[derive(Debug, Clone, uniffi::Record)]
pub struct MediaPlayer {
    pub player: String,
    pub name: String,
    pub state: PlaybackState,
    pub title: String,
    pub artist: String,
    pub album: String,
    pub length_ms: Option<u64>,
    /// When sent; advance it while playing.
    pub position_ms: u64,
    pub volume: Option<u8>,
    /// PNG or JPEG.
    pub artwork: Option<Vec<u8>>,
    pub can: Vec<MediaCommandKind>,
}

/// A fresh Ed25519 key as PKCS#8; the app keeps it encrypted by an Android Keystore key.
#[uniffi::export]
pub fn generate_identity() -> Result<Vec<u8>, LinkError> {
    Ok(Identity::generate()?.pkcs8().to_vec())
}

#[derive(uniffi::Object)]
pub struct LinkClient {
    /// Owns the worker thread the actor runs on; dropping the client stops both.
    runtime: tokio::runtime::Runtime,
    client: Client,
    /// Only [`LinkClient::next_event`] reads it; the lock serialises callers, it guards no state.
    events: Mutex<mpsc::Receiver<ClientEvent>>,
}

/// How the app reaches a desktop over Bluetooth: connect RFCOMM to `address` at Link's service UUID and return one
/// end of a socket pair the app pumps that connection through, detached. -1 when it cannot connect. Called off the
/// main thread, and may block.
#[uniffi::export(with_foreign)]
pub trait BluetoothLink: Send + Sync {
    fn open(&self, address: String) -> i32;
}

struct Opener(Arc<dyn BluetoothLink>);

impl link_core::stream::BluetoothOpener for Opener {
    fn open(&self, address: &str) -> std::io::Result<OwnedFd> {
        let fd = self.0.open(address.to_owned());
        if fd < 0 {
            return Err(std::io::Error::new(std::io::ErrorKind::NotConnected, "Bluetooth did not connect"));
        }
        // SAFETY: the app hands over a descriptor it detached and no longer uses, so nothing else owns or closes it.
        Ok(unsafe { OwnedFd::from_raw_fd(fd) })
    }
}

#[derive(Debug, Clone, uniffi::Record)]
pub struct BrowseRoot {
    pub name: String,
    pub path: String,
}

/// A local-only hotspot's credentials, from [`PhoneHotspot::start`].
#[derive(Debug, Clone, uniffi::Record)]
pub struct HotspotCredentials {
    pub ssid: String,
    pub passphrase: String,
}

/// How the app starts and stops a local-only hotspot when a transfer is too large for Bluetooth. `start` blocks until
/// the hotspot is up and returns None when Android refused it. Called off the main thread.
#[uniffi::export(with_foreign)]
pub trait PhoneHotspot: Send + Sync {
    fn start(&self) -> Option<HotspotCredentials>;
    fn stop(&self);
}

struct Hotspots(Arc<dyn PhoneHotspot>);

impl link_core::hotspot::HotspotProvider for Hotspots {
    fn start(&self) -> std::io::Result<link_core::proto::message::Hotspot> {
        let started = self.0.start().ok_or_else(|| std::io::Error::other("Android did not start the hotspot"))?;
        Ok(link_core::proto::message::Hotspot { ssid: started.ssid, passphrase: started.passphrase })
    }

    fn stop(&self) {
        self.0.stop();
    }
}

#[uniffi::export]
impl LinkClient {
    /// `identity` is PKCS#8 from [`generate_identity`]; `store_path` is where paired desktops are kept, and transfer
    /// state beside it; received files are written to `incoming_dir` before the app publishes them.
    #[uniffi::constructor]
    pub fn new(
        identity: Vec<u8>,
        store_path: String,
        name: String,
        incoming_dir: String,
        bluetooth: Option<Arc<dyn BluetoothLink>>,
        hotspot: Option<Arc<dyn PhoneHotspot>>,
    ) -> Result<Arc<Self>, LinkError> {
        // The core's logs, which are otherwise lost on Android, go to logcat under "link".
        #[cfg(target_os = "android")]
        android_logger::init_once(
            android_logger::Config::default().with_max_level(log::LevelFilter::Info).with_tag("link"),
        );
        let store_path = PathBuf::from(store_path);
        let state = store_path.parent().map(Path::to_path_buf).unwrap_or_default();
        let inbox = Inbox::new(PathBuf::from(incoming_dir), &state)?;
        let runtime = tokio::runtime::Builder::new_multi_thread()
            .worker_threads(1)
            .thread_name("link")
            .enable_all()
            .build()
            .map_err(|error| LinkError::Failed { reason: error.to_string() })?;
        let phone = {
            let _entered = runtime.enter();
            let mut phone = Phone::new(Identity::from_pkcs8(identity)?, store_path, name, None)?;
            if let Some(link) = bluetooth {
                phone.set_bluetooth(Arc::new(Opener(link)));
            }
            if let Some(hotspot) = hotspot {
                phone.set_hotspot(Arc::new(Hotspots(hotspot)));
            }
            phone
        };
        let (client, actor, events) = client::client(phone, inbox);
        runtime.spawn(actor.run());
        Ok(Arc::new(Self { runtime, client, events: Mutex::new(events) }))
    }

    /// The folders a desktop whose browse switch is on sees, by name (`Storage`, `Photos`); an empty list while the
    /// app lacks All files access.
    pub async fn set_browse_roots(&self, roots: Vec<BrowseRoot>) -> Result<(), LinkError> {
        let roots = link_core::browse::Roots::new(
            roots.into_iter().map(|root| (root.name, PathBuf::from(root.path))).collect(),
        );
        let client = self.client.clone();
        self.run(async move { client.set_browse_roots(roots).await }).await
    }

    /// Pairs from the QR code's URI and keeps the session open.
    pub async fn pair_uri(&self, uri: String) -> Result<Desktop, LinkError> {
        let target = PairTarget::Uri(PairingUri::parse(&uri)?);
        let client = self.client.clone();
        self.run(async move { client.pair(target).await }).await.map(|peer| describe(&peer, true, false))
    }

    /// Pairs with a typed code. `addresses` may be empty: the core then finds the pairing desktop by mDNS.
    pub async fn pair_code(&self, code: String, addresses: Vec<String>) -> Result<Desktop, LinkError> {
        let candidates = addresses.iter().filter_map(|text| text.parse().ok()).collect();
        let target = PairTarget::Code { code, candidates };
        let client = self.client.clone();
        self.run(async move { client.pair(target).await }).await.map(|peer| describe(&peer, true, false))
    }

    /// Opens a session now unless one is live.
    pub async fn connect(&self, id: String) -> Result<(), LinkError> {
        let (client, id) = (self.client.clone(), parse_id(&id)?);
        self.run(async move { client.connect(id).await }).await
    }

    /// While present, every paired desktop stays connected with keep-alive and is redialled when it drops.
    pub async fn set_present(&self, present: bool) -> Result<(), LinkError> {
        let client = self.client.clone();
        self.run(async move { client.set_present(present).await }).await
    }

    /// Sends text or a link, connecting first if needed, and returns once the desktop acknowledged it.
    pub async fn share(&self, desktop_id: String, kind: ShareKind, text: String) -> Result<(), LinkError> {
        let (client, id) = (self.client.clone(), parse_id(&desktop_id)?);
        let kind = match kind {
            ShareKind::Text => message::ShareKind::Text,
            ShareKind::Link => message::ShareKind::Link,
        };
        self.run(async move { client.share(id, Share { kind, text }).await }).await
    }

    /// Returns whether the desktop was told; the desktop is forgotten either way.
    pub async fn unpair(&self, id: String) -> Result<bool, LinkError> {
        let (client, id) = (self.client.clone(), parse_id(&id)?);
        self.run(async move { client.unpair(id).await }).await
    }

    pub async fn desktops(&self) -> Result<Vec<Desktop>, LinkError> {
        let client = self.client.clone();
        let states = self.run(async move { client.desktops().await }).await?;
        Ok(states.iter().map(|state| describe(&state.peer, state.connected, state.bluetooth)).collect())
    }

    pub async fn set_sharing(&self, desktop_id: String, feature: Feature, on: bool) -> Result<(), LinkError> {
        let (client, id) = (self.client.clone(), parse_id(&desktop_id)?);
        let feature = match feature {
            Feature::Clipboard => store::Feature::Clipboard,
            Feature::Files => store::Feature::Files,
            Feature::Notifications => store::Feature::Notifications,
            Feature::Media => store::Feature::Media,
            Feature::Ring => store::Feature::Ring,
            Feature::Calls => store::Feature::Calls,
            Feature::Browse => store::Feature::Browse,
        };
        self.run(async move { client.set_sharing(id, feature, on).await }).await
    }

    /// Mirrors a notification to every connected desktop that takes notifications; returns how many.
    pub async fn post_notification(&self, notification: PhoneNotification) -> Result<u32, LinkError> {
        let PhoneNotification { id, app, title, text, icon, actions } = notification;
        let actions = actions
            .into_iter()
            .map(|action| message::NotificationButton { id: action.id, label: action.label, reply: action.reply })
            .collect();
        let posted = message::NotificationPosted { id, app, title, text, icon, actions };
        self.broadcast(Message::NotificationPosted(posted)).await
    }

    pub async fn remove_notification(&self, id: String) -> Result<u32, LinkError> {
        self.broadcast(Message::NotificationRemoved(message::NotificationRemoved { id })).await
    }

    /// Describes this phone's player to every connected desktop that takes media; returns how many.
    pub async fn publish_player(&self, player: MediaPlayer) -> Result<u32, LinkError> {
        self.broadcast(Message::MediaPlayer(player.into())).await
    }

    pub async fn player_gone(&self, player: String) -> Result<u32, LinkError> {
        self.broadcast(Message::MediaGone(message::MediaGone { player })).await
    }

    /// Reports the call state to every connected desktop that takes calls; `number` and `name` only when known.
    pub async fn report_call(
        &self,
        state: CallState,
        number: Option<String>,
        name: Option<String>,
    ) -> Result<u32, LinkError> {
        let state = match state {
            CallState::Ringing => message::CallState::Ringing,
            CallState::Active => message::CallState::Active,
            CallState::Idle => message::CallState::Idle,
        };
        self.broadcast(Message::Call(message::Call { state, number, name })).await
    }

    /// Rings a desktop, or stops it, connecting first if needed.
    pub async fn ring_desktop(&self, desktop_id: String, on: bool) -> Result<(), LinkError> {
        let (client, id) = (self.client.clone(), parse_id(&desktop_id)?);
        self.run(async move { client.send(id, Message::Ring(message::Ring { on })).await }).await
    }

    /// Reports this phone's ringing to every connected desktop that may ring it.
    pub async fn report_ringing(&self, on: bool) -> Result<u32, LinkError> {
        self.broadcast(Message::Ringing(message::Ringing { on })).await
    }

    /// Commands one of a desktop's players, connecting first if needed.
    pub async fn media_command(
        &self,
        desktop_id: String,
        player: String,
        command: MediaCommandKind,
        value: Option<u64>,
    ) -> Result<(), LinkError> {
        let (client, id) = (self.client.clone(), parse_id(&desktop_id)?);
        let command = message::MediaCommand { player, command: command.into(), value };
        self.run(async move { client.send(id, Message::MediaCommand(command)).await }).await
    }

    /// The next event, waiting until there is one; `None` once the client has stopped.
    pub async fn next_event(&self) -> Option<LinkEvent> {
        let mut events = self.events.lock().await;
        loop {
            if let Some(event) = translate(events.recv().await?) {
                return Some(event);
            }
        }
    }

    /// Offers files to the desktop, connecting first if needed; returns the transfer id once the offer is on its way.
    pub async fn send_files(&self, desktop_id: String, files: Vec<OutgoingFile>) -> Result<String, LinkError> {
        let (client, id) = (self.client.clone(), parse_id(&desktop_id)?);
        let sources = files.into_iter().map(source).collect::<Result<Vec<_>, _>>()?;
        self.run(async move { client.send_files(id, sources).await }).await.map(TransferId::to_hex)
    }

    /// The phone's battery (percent) and network (wifi, cellular, ethernet, none, or other), sent to every desktop on
    /// connect and, at most every 10 s, on change.
    pub async fn set_status(&self, battery: u8, charging: bool, network: String) -> Result<(), LinkError> {
        let network = message::NetworkKind::parse(&network)
            .ok_or_else(|| LinkError::Rejected { reason: format!("unknown network {network}") })?;
        let status = message::Status { battery: battery.min(100), charging, network };
        let client = self.client.clone();
        self.run(async move { client.set_status(status).await }).await
    }

    /// Offers text from the phone's clipboard to every connected desktop, inline.
    pub async fn offer_clip_text(&self, text: String) -> Result<(), LinkError> {
        let client = self.client.clone();
        let clip = LocalClip { mimes: vec!["text/plain;charset=utf-8".to_owned()], text: Some(text), data: None };
        self.run(async move { client.offer_clip(clip).await }).await
    }

    /// Writes `mime` of a desktop's clipboard offer into `fd`, which the client takes over (a pipe's write end a
    /// content provider hands out), and returns the byte count.
    pub async fn pull_clip(&self, desktop_id: String, clip_id: u64, mime: String, fd: i32) -> Result<u64, LinkError> {
        let (client, id) = (self.client.clone(), parse_id(&desktop_id)?);
        if fd < 0 {
            return Err(LinkError::Rejected { reason: "not a file descriptor".to_owned() });
        }
        // SAFETY: the app hands over a descriptor it detached and no longer uses, so nothing else owns or closes it.
        let sink = File::from(unsafe { OwnedFd::from_raw_fd(fd) });
        self.run(async move { client.pull_clip(id, clip_id, mime, sink).await }).await
    }

    /// False when the offer no longer waits for an answer.
    pub async fn accept_transfer(&self, transfer_id: String) -> Result<bool, LinkError> {
        let (client, id) = (self.client.clone(), parse_transfer(&transfer_id)?);
        self.run(async move { client.decide(id, true).await }).await
    }

    pub async fn decline_transfer(&self, transfer_id: String) -> Result<bool, LinkError> {
        let (client, id) = (self.client.clone(), parse_transfer(&transfer_id)?);
        self.run(async move { client.decide(id, false).await }).await
    }

    pub async fn cancel_transfer(&self, transfer_id: String) -> Result<bool, LinkError> {
        let (client, id) = (self.client.clone(), parse_transfer(&transfer_id)?);
        self.run(async move { client.cancel_transfer(id).await }).await
    }
}

fn transfer_event(event: TransferEvent) -> Option<LinkEvent> {
    Some(match event {
        TransferEvent::Offered { id, from, files } => LinkEvent::TransferOffered {
            transfer_id: id.to_hex(),
            desktop_id: from.to_string(),
            files: files
                .into_iter()
                .map(|file| OfferedFile { name: file.name, size: file.size, mime: file.mime })
                .collect(),
        },
        TransferEvent::Progress { id, bytes, total } => {
            LinkEvent::TransferProgress { transfer_id: id.to_hex(), bytes, total }
        }
        TransferEvent::Finished { id, peer, incoming, status, files } => LinkEvent::TransferFinished {
            transfer_id: id.to_hex(),
            desktop_id: peer.to_string(),
            incoming,
            status: status.as_str().to_owned(),
            files: files
                .into_iter()
                .map(|file| ReceivedFile { path: file.path.to_string_lossy().into_owned(), sha256: hex(&file.sha256) })
                .collect(),
        },
        TransferEvent::ClipOffered { from, id, mimes, size, text } => {
            LinkEvent::ClipOffered { desktop_id: from.to_string(), clip_id: id, mimes, size, text }
        }
        TransferEvent::Busy { .. } => return None,
    })
}

/// Takes ownership of the app's descriptor, which the app detached from its `ParcelFileDescriptor`.
fn source(file: OutgoingFile) -> Result<Source, LinkError> {
    if file.fd < 0 {
        return Err(LinkError::Rejected { reason: "not a file descriptor".to_owned() });
    }
    // SAFETY: the app hands over a descriptor it detached and no longer uses, so nothing else owns or closes it.
    let owned = unsafe { OwnedFd::from_raw_fd(file.fd) };
    let source = Source::new(File::from(owned), file.name, file.mime)?;
    if source.size() != file.size {
        return Err(LinkError::Rejected { reason: "the file's size is not what the app reported".to_owned() });
    }
    Ok(source)
}

fn parse_transfer(id: &str) -> Result<TransferId, LinkError> {
    TransferId::parse_hex(id).ok_or_else(|| LinkError::Rejected { reason: "not a transfer id".to_owned() })
}

fn hex(bytes: &[u8]) -> String {
    bytes.iter().fold(String::with_capacity(2 * bytes.len()), |mut hex, byte| {
        let _ = write!(hex, "{byte:02x}");
        hex
    })
}

impl LinkClient {
    async fn broadcast(&self, message: Message) -> Result<u32, LinkError> {
        let client = self.client.clone();
        let sent = self.run(async move { client.broadcast(message).await }).await?;
        Ok(u32::try_from(sent).unwrap_or(u32::MAX))
    }
}

fn translate(event: ClientEvent) -> Option<LinkEvent> {
    Some(match event {
        ClientEvent::Connected { desktop, .. } => LinkEvent::Connected { desktop_id: desktop.id.to_string() },
        ClientEvent::Disconnected { id, .. } => LinkEvent::Disconnected { desktop_id: id.to_string() },
        ClientEvent::Received { from, share } => LinkEvent::Received {
            desktop_id: from.to_string(),
            kind: match share.kind {
                message::ShareKind::Text => ShareKind::Text,
                message::ShareKind::Link => ShareKind::Link,
            },
            text: share.text,
        },
        ClientEvent::Unpaired { id } => LinkEvent::Unpaired { desktop_id: id.to_string() },
        ClientEvent::Transfer(event) => return transfer_event(event),
        ClientEvent::Message { from, message } => {
            let desktop_id = from.to_string();
            match message {
                Message::NotificationAction(action) => LinkEvent::NotificationAction {
                    desktop_id,
                    id: action.id,
                    action: action.action,
                    reply_text: action.reply_text,
                },
                Message::NotificationDismiss(dismiss) => {
                    LinkEvent::NotificationDismissed { desktop_id, id: dismiss.id }
                }
                Message::MediaPlayer(player) => LinkEvent::PlayerChanged { desktop_id, player: player.into() },
                Message::MediaGone(gone) => LinkEvent::PlayerGone { desktop_id, player: gone.player },
                Message::Ring(ring) => LinkEvent::RingRequested { desktop_id, on: ring.on },
                Message::CallAction(action) => LinkEvent::CallActionRequested {
                    desktop_id,
                    action: match action.action {
                        message::CallActionKind::Mute => CallAction::Mute,
                        message::CallActionKind::Decline => CallAction::Decline,
                    },
                },
                Message::Ringing(ringing) => LinkEvent::DesktopRinging { desktop_id, on: ringing.on },
                Message::MediaCommand(command) => LinkEvent::PlayerCommand {
                    desktop_id,
                    player: command.player,
                    command: command.command.into(),
                    value: command.value,
                },
                _ => return None,
            }
        }
    })
}

impl LinkClient {
    /// Runs `work` on the client's runtime, which owns the timers and sockets it needs.
    async fn run<T: Send + 'static>(
        &self,
        work: impl Future<Output = Result<T, link_core::Error>> + Send + 'static,
    ) -> Result<T, LinkError> {
        let joined = self.runtime.spawn(work).await.map_err(|_| stopped())?;
        Ok(joined?)
    }
}

fn describe(peer: &Peer, connected: bool, bluetooth: bool) -> Desktop {
    let store::Grants { clipboard, files, notifications, media, ring, calls, browse } = peer.grants;
    Desktop {
        id: peer.id.to_string(),
        name: peer.name.clone(),
        addresses: peer.addresses.iter().map(ToString::to_string).collect(),
        last_seen: peer.last_seen,
        connected,
        bluetooth,
        sharing: Sharing { clipboard, files, notifications, media, ring, calls, browse },
    }
}

fn parse_id(id: &str) -> Result<DeviceId, LinkError> {
    Ok(DeviceId::parse(id)?)
}

fn stopped() -> LinkError {
    LinkError::Failed { reason: "the link runtime stopped".to_owned() }
}

impl From<MediaCommandKind> for message::MediaCommandKind {
    fn from(kind: MediaCommandKind) -> Self {
        match kind {
            MediaCommandKind::Play => Self::Play,
            MediaCommandKind::Pause => Self::Pause,
            MediaCommandKind::PlayPause => Self::PlayPause,
            MediaCommandKind::Next => Self::Next,
            MediaCommandKind::Previous => Self::Previous,
            MediaCommandKind::Seek => Self::Seek,
            MediaCommandKind::Volume => Self::Volume,
        }
    }
}

impl From<message::MediaCommandKind> for MediaCommandKind {
    fn from(kind: message::MediaCommandKind) -> Self {
        match kind {
            message::MediaCommandKind::Play => Self::Play,
            message::MediaCommandKind::Pause => Self::Pause,
            message::MediaCommandKind::PlayPause => Self::PlayPause,
            message::MediaCommandKind::Next => Self::Next,
            message::MediaCommandKind::Previous => Self::Previous,
            message::MediaCommandKind::Seek => Self::Seek,
            message::MediaCommandKind::Volume => Self::Volume,
        }
    }
}

impl From<MediaPlayer> for message::MediaPlayer {
    fn from(player: MediaPlayer) -> Self {
        let state = match player.state {
            PlaybackState::Playing => message::PlaybackState::Playing,
            PlaybackState::Paused => message::PlaybackState::Paused,
            PlaybackState::Stopped => message::PlaybackState::Stopped,
        };
        Self {
            player: player.player,
            name: player.name,
            state,
            title: player.title,
            artist: player.artist,
            album: player.album,
            length_ms: player.length_ms,
            position_ms: player.position_ms,
            volume: player.volume,
            artwork: player.artwork,
            can: player.can.into_iter().map(Into::into).collect(),
        }
    }
}

impl From<message::MediaPlayer> for MediaPlayer {
    fn from(player: message::MediaPlayer) -> Self {
        let state = match player.state {
            message::PlaybackState::Playing => PlaybackState::Playing,
            message::PlaybackState::Paused => PlaybackState::Paused,
            message::PlaybackState::Stopped => PlaybackState::Stopped,
        };
        Self {
            player: player.player,
            name: player.name,
            state,
            title: player.title,
            artist: player.artist,
            album: player.album,
            length_ms: player.length_ms,
            position_ms: player.position_ms,
            volume: player.volume,
            artwork: player.artwork,
            can: player.can.into_iter().map(Into::into).collect(),
        }
    }
}
