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

use link_proto::message::Message;

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
    /// What this device may do with the other: on a desktop, what the phone may send it; on a phone, what it shares
    /// with that desktop. Named `sharing` in phone stores from before files and clipboard.
    #[serde(default, alias = "sharing")]
    pub grants: Grants,
    /// The desktop's Bluetooth adapter, from its hello; the phone dials it when no IP path answers.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub bluetooth: Option<String>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Feature {
    Clipboard,
    Files,
    Notifications,
    Media,
    Ring,
    Calls,
    /// The desktop may list and read the phone's shared storage; a phone-side switch, off after pairing.
    Browse,
}

/// One switch per feature and paired device, all on after pairing but `browse`. A store from before a feature gets it on.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields, default)]
#[expect(clippy::struct_excessive_bools, reason = "one independent switch per feature")]
pub struct Grants {
    pub clipboard: bool,
    pub files: bool,
    pub notifications: bool,
    pub media: bool,
    pub ring: bool,
    pub calls: bool,
    pub browse: bool,
}

impl Default for Grants {
    fn default() -> Self {
        Self { clipboard: true, files: true, notifications: true, media: true, ring: true, calls: true, browse: false }
    }
}

impl Feature {
    pub const ALL: [Self; 7] =
        [Self::Clipboard, Self::Files, Self::Notifications, Self::Media, Self::Ring, Self::Calls, Self::Browse];

    pub fn as_str(self) -> &'static str {
        match self {
            Self::Clipboard => "clipboard",
            Self::Files => "files",
            Self::Notifications => "notifications",
            Self::Media => "media",
            Self::Ring => "ring",
            Self::Calls => "calls",
            Self::Browse => "browse",
        }
    }

    pub fn parse(name: &str) -> Option<Self> {
        Self::ALL.into_iter().find(|feature| feature.as_str() == name)
    }
}

/// The grant that governs a message; `None` for the ones every paired device may send.
pub fn feature_of(message: &Message) -> Option<Feature> {
    match message {
        Message::NotificationPosted(_)
        | Message::NotificationRemoved(_)
        | Message::NotificationAction(_)
        | Message::NotificationDismiss(_) => Some(Feature::Notifications),
        Message::MediaPlayer(_) | Message::MediaGone(_) | Message::MediaCommand(_) => Some(Feature::Media),
        Message::Ring(_) | Message::Ringing(_) => Some(Feature::Ring),
        Message::Call(_) | Message::CallAction(_) => Some(Feature::Calls),
        Message::ClipOffer(_) | Message::ClipPull(_) | Message::ClipData(_) => Some(Feature::Clipboard),
        Message::Offer(_)
        | Message::HotspotRequest
        | Message::Hotspot(_)
        | Message::HotspotJoined(_)
        | Message::HotspotEnd(_) => Some(Feature::Files),
        _ => None,
    }
}

impl Grants {
    pub fn allows(self, feature: Feature) -> bool {
        match feature {
            Feature::Clipboard => self.clipboard,
            Feature::Files => self.files,
            Feature::Notifications => self.notifications,
            Feature::Media => self.media,
            Feature::Ring => self.ring,
            Feature::Calls => self.calls,
            Feature::Browse => self.browse,
        }
    }

    pub fn set(&mut self, feature: Feature, on: bool) {
        let flag = match feature {
            Feature::Clipboard => &mut self.clipboard,
            Feature::Files => &mut self.files,
            Feature::Notifications => &mut self.notifications,
            Feature::Media => &mut self.media,
            Feature::Ring => &mut self.ring,
            Feature::Calls => &mut self.calls,
            Feature::Browse => &mut self.browse,
        };
        *flag = on;
    }

    /// The granted features' names, as D-Bus lists them.
    pub fn names(self) -> Vec<String> {
        Feature::ALL
            .into_iter()
            .filter(|feature| self.allows(*feature))
            .map(|feature| feature.as_str().to_owned())
            .collect()
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
    /// "Visible to `LocalSend`": the desktop's `LocalSend` backend listens only while this is on.
    #[serde(default, skip_serializing_if = "std::ops::Not::not")]
    pub localsend: bool,
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
            bluetooth: None,
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
