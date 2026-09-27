//! Kotlin bindings of the phone role for the Android app (`UniFFI`). One `LinkClient` owns a single-worker tokio
//! runtime and an actor holding the `Phone` and its open sessions; every method is a suspend function in Kotlin.

use std::collections::HashMap;
use std::path::PathBuf;
use std::sync::Arc;

use link_core::identity::{DeviceId, Identity};
use link_core::phone::{PairTarget, Phone, Session};
use link_core::proto::CloseCode;
use link_core::proto::pairing::PairingError;
use link_core::store::Peer;
use link_core::uri::PairingUri;
use tokio::sync::{mpsc, oneshot};

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
    Failed { reason: String },
}

impl From<link_core::Error> for LinkError {
    fn from(error: link_core::Error) -> Self {
        use link_core::Error;
        match error {
            Error::Pairing(PairingError::Mismatch) | Error::Closed(CloseCode::PairingFailed) => Self::WrongCode,
            Error::Closed(CloseCode::Unpaired) => Self::Unpaired,
            Error::Unreachable | Error::Timeout => Self::Unreachable,
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

/// A fresh Ed25519 key as PKCS#8; the app keeps it encrypted by an Android Keystore key.
#[uniffi::export]
pub fn generate_identity() -> Result<Vec<u8>, LinkError> {
    Ok(Identity::generate()?.pkcs8().to_vec())
}

#[derive(uniffi::Object)]
pub struct LinkClient {
    /// Owns the worker thread the actor runs on; dropping the client stops both.
    _runtime: tokio::runtime::Runtime,
    commands: mpsc::Sender<Command>,
}

enum Command {
    Pair { target: PairTarget, reply: oneshot::Sender<Result<Desktop, LinkError>> },
    Connect { id: DeviceId, reply: oneshot::Sender<Result<Desktop, LinkError>> },
    Disconnect { id: DeviceId },
    Unpair { id: DeviceId, reply: oneshot::Sender<Result<bool, LinkError>> },
    Desktops { reply: oneshot::Sender<Vec<Desktop>> },
}

struct Actor {
    phone: Phone,
    sessions: HashMap<DeviceId, Session>,
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
        let (commands, receiver) = mpsc::channel(16);
        runtime.spawn(Actor { phone, sessions: HashMap::new() }.run(receiver));
        Ok(Arc::new(Self { _runtime: runtime, commands }))
    }

    /// Pairs from the QR code's URI and keeps the session open.
    pub async fn pair_uri(&self, uri: String) -> Result<Desktop, LinkError> {
        let target = PairTarget::Uri(PairingUri::parse(&uri)?);
        self.request(|reply| Command::Pair { target, reply }).await?
    }

    /// Pairs with a typed code. `addresses` may be empty: the core then finds the pairing desktop by mDNS.
    pub async fn pair_code(&self, code: String, addresses: Vec<String>) -> Result<Desktop, LinkError> {
        let candidates = addresses.iter().filter_map(|text| text.parse().ok()).collect();
        let target = PairTarget::Code { code, candidates };
        self.request(|reply| Command::Pair { target, reply }).await?
    }

    pub async fn connect(&self, id: String) -> Result<Desktop, LinkError> {
        let id = parse_id(&id)?;
        self.request(|reply| Command::Connect { id, reply }).await?
    }

    pub async fn disconnect(&self, id: String) -> Result<(), LinkError> {
        let id = parse_id(&id)?;
        self.commands.send(Command::Disconnect { id }).await.map_err(|_| stopped())
    }

    /// Returns whether the desktop was told; the desktop is forgotten either way.
    pub async fn unpair(&self, id: String) -> Result<bool, LinkError> {
        let id = parse_id(&id)?;
        self.request(|reply| Command::Unpair { id, reply }).await?
    }

    pub async fn desktops(&self) -> Result<Vec<Desktop>, LinkError> {
        self.request(|reply| Command::Desktops { reply }).await
    }
}

impl LinkClient {
    async fn request<T>(&self, make: impl FnOnce(oneshot::Sender<T>) -> Command) -> Result<T, LinkError> {
        let (reply, response) = oneshot::channel();
        self.commands.send(make(reply)).await.map_err(|_| stopped())?;
        response.await.map_err(|_| stopped())
    }
}

impl Actor {
    async fn run(mut self, mut commands: mpsc::Receiver<Command>) {
        while let Some(command) = commands.recv().await {
            match command {
                Command::Pair { target, reply } => drop(reply.send(self.pair(target).await)),
                Command::Connect { id, reply } => drop(reply.send(self.connect(id).await)),
                Command::Disconnect { id } => {
                    if let Some(session) = self.sessions.remove(&id) {
                        session.close();
                    }
                }
                Command::Unpair { id, reply } => {
                    self.sessions.remove(&id);
                    drop(reply.send(self.phone.unpair(&id).await.map_err(LinkError::from)));
                }
                Command::Desktops { reply } => drop(reply.send(self.desktops())),
            }
        }
        self.phone.finish().await;
    }

    async fn pair(&mut self, target: PairTarget) -> Result<Desktop, LinkError> {
        let session = self.phone.pair(target).await?;
        Ok(self.hold(session))
    }

    async fn connect(&mut self, id: DeviceId) -> Result<Desktop, LinkError> {
        if let Some(session) = self.sessions.get(&id).filter(|session| session.connection.close_reason().is_none()) {
            return Ok(self.describe(&session.desktop));
        }
        let session = self.phone.connect(&id).await?;
        Ok(self.hold(session))
    }

    fn hold(&mut self, session: Session) -> Desktop {
        let id = session.desktop.id.clone();
        self.sessions.insert(id.clone(), session);
        let peer = self.sessions[&id].desktop.clone();
        self.describe(&peer)
    }

    fn desktops(&self) -> Vec<Desktop> {
        self.phone.desktops().iter().map(|peer| self.describe(peer)).collect()
    }

    fn describe(&self, peer: &Peer) -> Desktop {
        let connected = self.sessions.get(&peer.id).is_some_and(|session| session.connection.close_reason().is_none());
        Desktop {
            id: peer.id.to_string(),
            name: peer.name.clone(),
            addresses: peer.addresses.iter().map(ToString::to_string).collect(),
            last_seen: peer.last_seen,
            connected,
        }
    }
}

fn parse_id(id: &str) -> Result<DeviceId, LinkError> {
    Ok(DeviceId::parse(id)?)
}

fn stopped() -> LinkError {
    LinkError::Failed { reason: "the link runtime stopped".to_owned() }
}
