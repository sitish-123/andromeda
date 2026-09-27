//! Kotlin bindings of the phone role for the Android app (`UniFFI`). One `LinkClient` owns a single-worker tokio
//! runtime running `link_core::client`'s actor; every method is a suspend function in Kotlin whose work runs there.

use std::path::PathBuf;
use std::sync::Arc;

use link_core::client::{self, Client, ClientEvent, DesktopState};
use link_core::identity::{DeviceId, Identity};
use link_core::phone::{PairTarget, Phone};
use link_core::proto::CloseCode;
use link_core::proto::message::{self, Share};
use link_core::proto::pairing::PairingError;
use link_core::store::Peer;
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

    /// The next event, waiting until there is one; `None` once the client has stopped.
    pub async fn next_event(&self) -> Option<LinkEvent> {
        let event = self.events.lock().await.recv().await?;
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
        })
    }
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
    Desktop {
        id: peer.id.to_string(),
        name: peer.name.clone(),
        addresses: peer.addresses.iter().map(ToString::to_string).collect(),
        last_seen: peer.last_seen,
        connected,
    }
}

fn parse_id(id: &str) -> Result<DeviceId, LinkError> {
    Ok(DeviceId::parse(id)?)
}

fn stopped() -> LinkError {
    LinkError::Failed { reason: "the link runtime stopped".to_owned() }
}
