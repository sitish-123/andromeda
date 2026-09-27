//! Kotlin bindings of the phone role for the Android app (`UniFFI`). One `LinkClient` owns a single-worker tokio
//! runtime running `link_core::client`'s actor; every method is a suspend function in Kotlin whose work runs there.

use std::fmt::Write as _;
use std::fs::File;
use std::os::fd::{FromRawFd, OwnedFd};
use std::path::{Path, PathBuf};
use std::sync::Arc;

use link_core::client::{self, Client, ClientEvent, DesktopState};
use link_core::identity::{DeviceId, Identity};
use link_core::inbox::Inbox;
use link_core::phone::{PairTarget, Phone};
use link_core::proto::CloseCode;
use link_core::proto::message::{self, Share, TransferId};
use link_core::proto::pairing::PairingError;
use link_core::store::Peer;
use link_core::transfer::{Source, TransferEvent};
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
    /// The desktop offers files; answer with `accept_transfer` or `decline_transfer` within 120 s.
    TransferOffered {
        transfer_id: String,
        desktop_id: String,
        files: Vec<OfferedFile>,
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
    /// `identity` is PKCS#8 from [`generate_identity`]; `store_path` is where paired desktops are kept, and transfer
    /// state beside it; received files are written to `incoming_dir` before the app publishes them.
    #[uniffi::constructor]
    pub fn new(
        identity: Vec<u8>,
        store_path: String,
        name: String,
        incoming_dir: String,
    ) -> Result<Arc<Self>, LinkError> {
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
            Phone::new(Identity::from_pkcs8(identity)?, store_path, name, None)?
        };
        let (client, actor, events) = client::client(phone, inbox);
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
        let mut events = self.events.lock().await;
        loop {
            if let Some(event) = describe_event(events.recv().await?) {
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

fn describe_event(event: ClientEvent) -> Option<LinkEvent> {
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
    })
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
