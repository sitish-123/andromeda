//! The phone role: pairing with desktops, reaching them, and the sessions it opens. Shared by the headless phone and
//! the Android app.

use std::net::SocketAddr;
use std::path::PathBuf;
use std::sync::Arc;

use link_proto::message::{Hello, Message, PairMethod, bluetooth_address};
use link_proto::pairing::Secret;
use link_proto::{CloseCode, VERSION};

use crate::browse::Roots;
use crate::control::{Control, Tap};
use crate::hotspot::HotspotProvider;
use crate::identity::{DeviceId, Identity};
use crate::pairing::pair_as_client;
use crate::reach::{self, Reached, Via};
use crate::store::{Feature, Peer, Store};
use crate::stream::{BluetoothOpener, FdStream, StreamDialer};
use crate::tls::{self, ServerPin};
use crate::transport::{Dialer, KEEP_ALIVE};
use crate::uri::PairingUri;
use crate::wire::Connection;
use crate::{Error, discovery};

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
    pub connection: Connection,
    pub control: Control,
    pub desktop: Peer,
    /// None over Bluetooth.
    pub addr: Option<SocketAddr>,
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
    stream_dialer: StreamDialer,
    bluetooth: Option<Arc<dyn BluetoothOpener>>,
    hotspot: Option<Arc<dyn HotspotProvider>>,
    browse_roots: Roots,
    present: bool,
    tap: Option<Tap>,
}

impl Phone {
    pub fn new(identity: Identity, store_path: PathBuf, name: String, tap: Option<Tap>) -> Result<Self, Error> {
        let store = Store::load(&store_path)?;
        let dialer = Dialer::new(&identity)?;
        let present_dialer = dialer.present();
        let stream_dialer = StreamDialer::new(&identity)?;
        Ok(Self {
            identity,
            store,
            store_path,
            name,
            dialer,
            present_dialer,
            stream_dialer,
            bluetooth: None,
            hotspot: None,
            browse_roots: Roots::default(),
            present: false,
            tap,
        })
    }

    /// Whether connections opened from now on keep themselves alive.
    pub fn set_present(&mut self, present: bool) {
        self.present = present;
    }

    /// Lets sessions fall back to Bluetooth when no IP path answers.
    pub fn set_bluetooth(&mut self, opener: Arc<dyn BluetoothOpener>) {
        self.bluetooth = Some(opener);
    }

    /// Lets a Bluetooth session move to this phone's hotspot for transfers too large for Bluetooth.
    pub fn set_hotspot(&mut self, provider: Arc<dyn HotspotProvider>) {
        self.hotspot = Some(provider);
    }

    pub fn hotspot(&self) -> Option<Arc<dyn HotspotProvider>> {
        self.hotspot.clone()
    }

    /// The folders a desktop with the browse switch on sees at `/`.
    pub fn set_browse_roots(&mut self, roots: Roots) {
        self.browse_roots = roots;
    }

    pub fn browse_roots(&self) -> &Roots {
        &self.browse_roots
    }

    /// The dialer sessions opened now use, for a caller that reaches a desktop off the actor.
    pub fn dialer(&self) -> &Dialer {
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
        let quic = dialed.connection;
        let connection = Connection::from(quic.clone());
        let mut control = Control::open(&connection, self.tap.clone()).await?;
        let hello = control.hello_as_client(self.hello()).await?;
        pair_as_client(&quic, &mut control, &secret, method, self.identity.spki()).await?;
        let desktop = self.remember(&tls::peer_spki(quic.peer_identity())?, &hello, Some(addr))?;
        Ok(Session { connection, control, desktop, addr: Some(addr), via: Via::LastKnown, resumed: false })
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
                drop(session.connection.closed().await);
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

    /// Over IP when any address answers, else over Bluetooth when the desktop has an adapter and the platform can
    /// open one.
    async fn open_session(&mut self, peer: &Peer) -> Result<Session, Error> {
        let ip = match reach::reach(self.dialer(), peer).await {
            Ok(reached) => return self.adopt_reached(peer, reached).await,
            Err(error) => error,
        };
        let (Some(address), Some(opener)) = (peer.bluetooth.clone(), self.bluetooth.clone()) else { return Err(ip) };
        log::info!("{}: no IP path ({ip}); trying Bluetooth", peer.id);
        let fd = tokio::task::spawn_blocking(move || opener.open(&address)).await.map_err(|_| Error::Stopped)??;
        let keep_alive = self.present.then_some(KEEP_ALIVE);
        let (mux, desktop) =
            self.stream_dialer.connect(FdStream::new(fd)?, peer.spki()?.fingerprint(), keep_alive).await?;
        if desktop.device_id() != peer.id {
            return Err(Error::BadKey);
        }
        let connection = Connection::Stream(mux);
        let mut control = Control::open(&connection, self.tap.clone()).await?;
        let hello = control.hello_as_client(self.hello()).await?;
        let desktop = self.remember(&desktop, &hello, None)?;
        Ok(Session { connection, control, desktop, addr: None, via: Via::Bluetooth, resumed: false })
    }

    /// Finishes a session to a desktop reached over IP, by this phone or by a probe running beside it.
    pub async fn adopt_reached(&mut self, peer: &Peer, reached: Reached) -> Result<Session, Error> {
        let connection = Connection::from(reached.dialed.connection);
        let mut control = Control::open(&connection, self.tap.clone()).await?;
        let hello = control.hello_as_client(self.hello()).await?;
        let desktop = if reached.via == Via::Hotspot {
            // The phone's own hotspot is gone once it stops: neither the address it reached nor the ones the desktop
            // announces on it are worth dialling later.
            self.remember(&peer.spki()?, &Hello { addresses: Vec::new(), ..hello }, None)?
        } else {
            self.remember(&peer.spki()?, &hello, Some(reached.addr))?
        };
        Ok(Session {
            connection,
            control,
            desktop,
            addr: Some(reached.addr),
            via: reached.via,
            resumed: reached.dialed.resumed,
        })
    }

    fn hello(&self) -> Hello {
        Hello { version: VERSION, name: self.name.clone(), addresses: Vec::new() }
    }

    fn remember(
        &mut self,
        desktop: &crate::identity::Spki,
        hello: &Hello,
        addr: Option<SocketAddr>,
    ) -> Result<Peer, Error> {
        let id = desktop.device_id();
        let mut peer = self.store.peer(&id).cloned().unwrap_or_else(|| Peer::new(desktop, hello.name.clone()));
        peer.name.clone_from(&hello.name);
        if let Some(addr) = addr {
            peer.remember(addr);
        }
        peer.learn(hello.addresses.iter().filter_map(|text| text.parse().ok()));
        if let Some(bluetooth) = hello.addresses.iter().find_map(|text| bluetooth_address(text)) {
            peer.bluetooth = Some(bluetooth);
        }
        peer.touch();
        self.store.upsert(peer.clone());
        self.store.save(&self.store_path)?;
        Ok(peer)
    }

    pub fn set_sharing(&mut self, id: &DeviceId, feature: Feature, on: bool) -> Result<(), Error> {
        let peer = self.store.peer_mut(id).ok_or(Error::UnknownDevice)?;
        peer.grants.set(feature, on);
        self.store.save(&self.store_path)
    }

    pub fn forget(&mut self, id: &DeviceId) -> Result<(), Error> {
        self.store.remove(id);
        self.store.save(&self.store_path)
    }
}

impl Session {
    pub fn close(&self) {
        self.connection.close(CloseCode::Done);
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
