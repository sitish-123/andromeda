//! The device store, `devices.json`: paired peers, their last-known addresses, and the desktop's QUIC port.

use std::fs::{self, OpenOptions};
use std::io::Write;
use std::net::SocketAddr;
use std::os::unix::fs::OpenOptionsExt;
use std::path::Path;
use std::time::{SystemTime, UNIX_EPOCH};

use base64::Engine;
use base64::engine::general_purpose::STANDARD;
use serde::{Deserialize, Serialize};

use crate::Error;
use crate::identity::{DeviceId, Spki};

const MAX_ADDRESSES: usize = 8;

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Peer {
    pub id: DeviceId,
    pub name: String,
    key: String,
    /// Most recent success first.
    #[serde(default)]
    pub addresses: Vec<SocketAddr>,
    /// Unix seconds.
    #[serde(default)]
    pub last_seen: u64,
    /// Accept this device's file offers without asking; a desktop-side setting.
    #[serde(default, skip_serializing_if = "std::ops::Not::not")]
    pub auto_accept: bool,
    #[serde(default)]
    pub grants: Grants,
}

/// What a device may do here. A store from before grants gets the defaults.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields, default)]
pub struct Grants {
    pub clipboard: bool,
    pub files: bool,
    pub notifications: bool,
}

impl Default for Grants {
    fn default() -> Self {
        Self { clipboard: true, files: true, notifications: false }
    }
}

impl Grants {
    pub const FEATURES: [&str; 3] = ["clipboard", "files", "notifications"];

    /// The granted features' names, as D-Bus lists them.
    pub fn names(self) -> Vec<String> {
        let flags = [self.clipboard, self.files, self.notifications];
        Self::FEATURES.iter().zip(flags).filter(|(_, on)| *on).map(|(name, _)| (*name).to_owned()).collect()
    }

    /// Sets a feature by name; false for a name that is not a feature.
    pub fn set(&mut self, feature: &str, granted: bool) -> bool {
        let flag = match feature {
            "clipboard" => &mut self.clipboard,
            "files" => &mut self.files,
            "notifications" => &mut self.notifications,
            _ => return false,
        };
        *flag = granted;
        true
    }
}

#[derive(Debug, Default, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Store {
    #[serde(default)]
    pub port: u16,
    #[serde(default)]
    pub peers: Vec<Peer>,
    /// Keys unpaired here, told at their next contact.
    #[serde(default)]
    pub revoked: Vec<DeviceId>,
}

impl Peer {
    pub fn new(spki: &Spki, name: String) -> Self {
        Self {
            id: spki.device_id(),
            name,
            key: STANDARD.encode(spki.as_der()),
            addresses: Vec::new(),
            last_seen: 0,
            auto_accept: false,
            grants: Grants::default(),
        }
    }

    pub fn spki(&self) -> Result<Spki, Error> {
        Spki::from_der(STANDARD.decode(&self.key).map_err(|_| Error::BadKey)?)
    }

    /// Records `addr` as the most recent address that worked.
    pub fn remember(&mut self, addr: SocketAddr) {
        self.addresses.retain(|known| *known != addr);
        self.addresses.insert(0, addr);
        self.addresses.truncate(MAX_ADDRESSES);
    }

    /// Adds addresses the peer announced, after the ones already proven.
    pub fn learn(&mut self, announced: impl IntoIterator<Item = SocketAddr>) {
        for addr in announced {
            if !self.addresses.contains(&addr) && self.addresses.len() < MAX_ADDRESSES {
                self.addresses.push(addr);
            }
        }
    }

    pub fn touch(&mut self) {
        self.last_seen = SystemTime::now().duration_since(UNIX_EPOCH).map_or(0, |elapsed| elapsed.as_secs());
    }
}

impl Store {
    /// Loads `path`; a missing file is an empty store.
    pub fn load(path: &Path) -> Result<Self, Error> {
        match fs::read(path) {
            Ok(bytes) => Ok(serde_json::from_slice(&bytes)?),
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => Ok(Self::default()),
            Err(error) => Err(error.into()),
        }
    }

    /// Writes atomically: a mode-0600 temporary file renamed over `path`.
    pub fn save(&self, path: &Path) -> Result<(), Error> {
        let tmp = path.with_extension("tmp");
        let mut file = OpenOptions::new().write(true).create(true).truncate(true).mode(0o600).open(&tmp)?;
        file.write_all(&serde_json::to_vec_pretty(self)?)?;
        file.sync_all()?;
        fs::rename(&tmp, path)?;
        Ok(())
    }

    pub fn peer(&self, id: &DeviceId) -> Option<&Peer> {
        self.peers.iter().find(|peer| peer.id == *id)
    }

    pub fn peer_mut(&mut self, id: &DeviceId) -> Option<&mut Peer> {
        self.peers.iter_mut().find(|peer| peer.id == *id)
    }

    /// Inserts `peer`, replacing any entry with the same id, and clears its revocation.
    pub fn upsert(&mut self, peer: Peer) {
        self.revoked.retain(|id| *id != peer.id);
        match self.peer_mut(&peer.id) {
            Some(existing) => *existing = peer,
            None => self.peers.push(peer),
        }
    }

    pub fn remove(&mut self, id: &DeviceId) -> Option<Peer> {
        let index = self.peers.iter().position(|peer| peer.id == *id)?;
        Some(self.peers.remove(index))
    }
}
