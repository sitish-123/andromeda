//! Kotlin bindings of the phone role for the Android app (`UniFFI`). One `LinkClient` owns a single-worker tokio
//! runtime running `link_core::client`'s actor; every method is a suspend function in Kotlin whose work runs there.

use std::path::PathBuf;
use std::sync::Arc;

use link_core::client::{self, Client, ClientEvent, DesktopState};
use link_core::identity::{DeviceId, Identity};
use link_core::phone::{PairTarget, Phone};
use link_core::proto::CloseCode;
use link_core::proto::message::{self, Message, Share};
use link_core::proto::pairing::PairingError;
use link_core::store::{self, Peer};
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
    pub sharing: Sharing,
}

/// The phone's switches for one desktop.
#[derive(Debug, Clone, Copy, uniffi::Record)]
#[expect(clippy::struct_excessive_bools, reason = "one independent switch per feature")]
pub struct Sharing {
    pub notifications: bool,
    pub media: bool,
    pub ring: bool,
    pub calls: bool,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, uniffi::Enum)]
pub enum Feature {
    Notifications,
    Media,
    Ring,
    Calls,
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
    /// A desktop commands this phone's player.
    PlayerCommand {
        desktop_id: String,
        player: String,
        command: MediaCommandKind,
        value: Option<u64>,
    },
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

#[uniffi::export]
impl LinkClient {
    /// `identity` is PKCS#8 from [`generate_identity`]; `store_path` is where paired desktops are kept.
    #[uniffi::constructor]
    pub fn new(identity: Vec<u8>, store_path: String, name: String) -> Result<Arc<Self>, LinkError> {
        let runtime = tokio::runtime::Builder::new_multi_thread()
            .worker_threads(1)
            .thread_name("link")
            .enable_all()
            .build()
            .map_err(|error| LinkError::Failed { reason: error.to_string() })?;
        let phone = {
            let _entered = runtime.enter();
            Phone::new(Identity::from_pkcs8(identity)?, PathBuf::from(store_path), name, None)?
        };
        let (client, actor, events) = client::client(phone);
        runtime.spawn(actor.run());
        Ok(Arc::new(Self { runtime, client, events: Mutex::new(events) }))
    }

    /// Pairs from the QR code's URI and keeps the session open.
    pub async fn pair_uri(&self, uri: String) -> Result<Desktop, LinkError> {
        let target = PairTarget::Uri(PairingUri::parse(&uri)?);
        let client = self.client.clone();
        self.run(async move { client.pair(target).await }).await.map(|peer| describe(&peer, true))
    }

    /// Pairs with a typed code. `addresses` may be empty: the core then finds the pairing desktop by mDNS.
    pub async fn pair_code(&self, code: String, addresses: Vec<String>) -> Result<Desktop, LinkError> {
        let candidates = addresses.iter().filter_map(|text| text.parse().ok()).collect();
        let target = PairTarget::Code { code, candidates };
        let client = self.client.clone();
        self.run(async move { client.pair(target).await }).await.map(|peer| describe(&peer, true))
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
        Ok(states.iter().map(|DesktopState { peer, connected }| describe(peer, *connected)).collect())
    }

    pub async fn set_sharing(&self, desktop_id: String, feature: Feature, on: bool) -> Result<(), LinkError> {
        let (client, id) = (self.client.clone(), parse_id(&desktop_id)?);
        let feature = match feature {
            Feature::Notifications => store::Feature::Notifications,
            Feature::Media => store::Feature::Media,
            Feature::Ring => store::Feature::Ring,
            Feature::Calls => store::Feature::Calls,
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

fn describe(peer: &Peer, connected: bool) -> Desktop {
    let store::Sharing { notifications, media, ring, calls } = peer.sharing;
    Desktop {
        id: peer.id.to_string(),
        name: peer.name.clone(),
        addresses: peer.addresses.iter().map(ToString::to_string).collect(),
        last_seen: peer.last_seen,
        connected,
        sharing: Sharing { notifications, media, ring, calls },
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
