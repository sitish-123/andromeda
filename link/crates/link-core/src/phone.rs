//! The phone role: pairing with desktops, reaching them, and the sessions it opens. Shared by the headless phone and
//! the Android app.

use std::net::SocketAddr;
use std::path::PathBuf;

use link_proto::message::{Hello, Message, PairMethod};
use link_proto::pairing::Secret;
use link_proto::{CloseCode, VERSION};

use crate::control::{Control, Tap};
use crate::identity::{DeviceId, Identity};
use crate::pairing::pair_as_client;
use crate::reach::{self, Via};
use crate::store::{Peer, Store};
use crate::tls::{self, ServerPin};
use crate::transport::Dialer;
use crate::uri::PairingUri;
use crate::{Error, close, discovery};

pub enum PairTarget {
    /// The typed code, and where to find the desktop (empty: the one desktop advertising an open window).
    Code {
        code: String,
        candidates: Vec<SocketAddr>,
    },
    Uri(PairingUri),
}

/// A live connection to a desktop, after both hellos.
pub struct Session {
    pub connection: quinn::Connection,
    pub control: Control,
    pub desktop: Peer,
    pub addr: SocketAddr,
    pub via: Via,
    pub resumed: bool,
}

pub struct Phone {
    identity: Identity,
    store: Store,
    store_path: PathBuf,
    name: String,
    dialer: Dialer,
    present_dialer: Dialer,
    present: bool,
    tap: Option<Tap>,
}

impl Phone {
    pub fn new(identity: Identity, store_path: PathBuf, name: String, tap: Option<Tap>) -> Result<Self, Error> {
        let store = Store::load(&store_path)?;
        let dialer = Dialer::new(&identity)?;
        let present_dialer = dialer.present();
        Ok(Self { identity, store, store_path, name, dialer, present_dialer, present: false, tap })
    }

    /// Whether connections opened from now on keep themselves alive.
    pub fn set_present(&mut self, present: bool) {
        self.present = present;
    }

    fn dialer(&self) -> &Dialer {
        if self.present { &self.present_dialer } else { &self.dialer }
    }

    pub fn desktops(&self) -> &[Peer] {
        &self.store.peers
    }

    pub async fn pair(&mut self, target: PairTarget) -> Result<Session, Error> {
        let (candidates, pin, secret, method) = match target {
            PairTarget::Code { code, candidates } => {
                if code.len() != 6 || !code.bytes().all(|byte| byte.is_ascii_digit()) {
                    return Err(Error::BadCode);
                }
                let candidates = if candidates.is_empty() { pairing_desktop().await? } else { candidates };
                (candidates, ServerPin::Any, Secret::new(code.into_bytes()), PairMethod::Code)
            }
            PairTarget::Uri(uri) => {
                (uri.addresses, ServerPin::Key(uri.fingerprint), Secret::new(uri.secret.to_vec()), PairMethod::Qr)
            }
        };
        let (dialed, addr) = reach::race(self.dialer(), &candidates, pin).await?;
        let connection = dialed.connection;
        let mut control = Control::open(&connection, self.tap.clone()).await?;
        let hello = control.hello_as_client(self.hello()).await?;
        pair_as_client(&connection, &mut control, &secret, method, self.identity.spki()).await?;
        let desktop = self.remember(&tls::peer_spki(connection.peer_identity())?, &hello, addr)?;
        Ok(Session { connection, control, desktop, addr, via: Via::LastKnown, resumed: false })
    }

    /// Reaches a paired desktop. A desktop that answers `unpaired` is forgotten, as the protocol requires.
    pub async fn connect(&mut self, id: &DeviceId) -> Result<Session, Error> {
        let peer = self.store.peer(id).cloned().ok_or(Error::UnknownDevice)?;
        let result = self.open_session(&peer).await;
        if matches!(result, Err(Error::Closed(CloseCode::Unpaired))) {
            self.forget(id)?;
        }
        result
    }

    /// Tells the desktop, then forgets it. Returns whether the desktop was told; it is forgotten either way.
    pub async fn unpair(&mut self, id: &DeviceId) -> Result<bool, Error> {
        let told = match self.connect(id).await {
            Ok(mut session) => {
                session.control.send(Message::Unpair).await?;
                session.connection.closed().await;
                true
            }
            Err(error) => {
                log::info!("unpairing {id} without telling it: {error}");
                false
            }
        };
        self.forget(id)?;
        Ok(told)
    }

    /// Waits until every connection has sent its close; call before the process exits.
    pub async fn finish(&self) {
        self.dialer.finish().await;
    }

    async fn open_session(&mut self, peer: &Peer) -> Result<Session, Error> {
        let reached = reach::reach(self.dialer(), peer).await?;
        let connection = reached.dialed.connection;
        let mut control = Control::open(&connection, self.tap.clone()).await?;
        let hello = control.hello_as_client(self.hello()).await?;
        let desktop = self.remember(&peer.spki()?, &hello, reached.addr)?;
        Ok(Session {
            connection,
            control,
            desktop,
            addr: reached.addr,
            via: reached.via,
            resumed: reached.dialed.resumed,
        })
    }

    fn hello(&self) -> Hello {
        Hello { version: VERSION, name: self.name.clone(), addresses: Vec::new() }
    }

    fn remember(&mut self, desktop: &crate::identity::Spki, hello: &Hello, addr: SocketAddr) -> Result<Peer, Error> {
        let id = desktop.device_id();
        let mut peer = self.store.peer(&id).cloned().unwrap_or_else(|| Peer::new(desktop, hello.name.clone()));
        peer.name.clone_from(&hello.name);
        peer.remember(addr);
        peer.learn(hello.addresses.iter().filter_map(|text| text.parse().ok()));
        peer.touch();
        self.store.upsert(peer.clone());
        self.store.save(&self.store_path)?;
        Ok(peer)
    }

    pub fn forget(&mut self, id: &DeviceId) -> Result<(), Error> {
        self.store.remove(id);
        self.store.save(&self.store_path)
    }
}

impl Session {
    pub fn close(&self) {
        close(&self.connection, CloseCode::Done);
    }
}

/// Addresses of the one desktop advertising an open pairing window.
async fn pairing_desktop() -> Result<Vec<SocketAddr>, Error> {
    let open: Vec<_> =
        discovery::browse(reach::MDNS_WINDOW, None).await?.into_iter().filter(|desktop| desktop.pairing).collect();
    match open.as_slice() {
        [desktop] => Ok(desktop.addresses.clone()),
        [] => Err(Error::NoPairingDesktop),
        _ => Err(Error::ManyPairingDesktops),
    }
}
