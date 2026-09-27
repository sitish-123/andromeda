//! Media both ways, owned by the hub: the desktop's players as phones see them, and each phone's players, the latest
//! of which is exported over MPRIS.

use std::collections::{BTreeMap, HashMap};
use std::path::PathBuf;

use link_core::identity::DeviceId;
use link_core::proto::message::{MediaGone, MediaPlayer, Message};
use link_core::session::SessionHandle;

use crate::desktop_media::{DesktopMediaHandle, Request};
use crate::hub::HubHandle;
use crate::mpris::Exported;

/// Players kept per phone; a new one beyond this drops the least recently updated.
const MAX_PHONE_PLAYERS: usize = 8;

#[derive(Default)]
struct PhonePlayers {
    /// Least recently updated first; the last is exported.
    players: Vec<MediaPlayer>,
    exported: Option<Exported>,
}

pub struct Media {
    desktop: BTreeMap<String, MediaPlayer>,
    phones: HashMap<DeviceId, PhonePlayers>,
    requests: DesktopMediaHandle,
    art: PathBuf,
}

impl Media {
    pub fn new(requests: DesktopMediaHandle, art: PathBuf) -> Self {
        Self { desktop: BTreeMap::new(), phones: HashMap::new(), requests, art }
    }

    pub fn desktop_player<'a>(&mut self, player: MediaPlayer, sessions: impl Iterator<Item = &'a SessionHandle>) {
        self.desktop.insert(player.player.clone(), player.clone());
        post_all(sessions, &Message::MediaPlayer(player));
    }

    pub fn desktop_gone<'a>(&mut self, player: String, sessions: impl Iterator<Item = &'a SessionHandle>) {
        if self.desktop.remove(&player).is_some() {
            post_all(sessions, &Message::MediaGone(MediaGone { player }));
        }
    }

    /// A new session hears every desktop player.
    pub fn connected(&self, session: &SessionHandle) {
        for player in self.desktop.values() {
            post_all(std::iter::once(session), &Message::MediaPlayer(player.clone()));
        }
    }

    pub async fn request(&self, request: Request) {
        self.requests.request(request).await;
    }

    pub async fn on_phone_message(&mut self, hub: &HubHandle, device: &DeviceId, device_name: &str, message: Message) {
        let phone = self.phones.entry(device.clone()).or_default();
        match message {
            Message::MediaPlayer(player) => {
                phone.players.retain(|known| known.player != player.player);
                if phone.players.len() >= MAX_PHONE_PLAYERS {
                    phone.players.remove(0);
                }
                phone.players.push(player);
            }
            Message::MediaGone(gone) => phone.players.retain(|known| known.player != gone.player),
            Message::MediaCommand(command) => return self.requests.request(Request::Command(command)).await,
            other => return log::warn!("{device}: {} is not media", other.kind()),
        }
        let Some(latest) = phone.players.last().cloned() else {
            if let Some(exported) = phone.exported.take() {
                exported.close().await;
            }
            return;
        };
        let result = match &mut phone.exported {
            Some(exported) => exported.update(device, &latest).await,
            None => match Exported::start(hub.clone(), device.clone(), device_name, &latest, &self.art).await {
                Ok(exported) => {
                    phone.exported = Some(exported);
                    Ok(())
                }
                Err(error) => Err(error),
            },
        };
        if let Err(error) = result {
            log::warn!("{device}: exporting its player: {error}");
        }
    }

    /// The phone's players go with its session.
    pub async fn disconnected(&mut self, device: &DeviceId) {
        if let Some(PhonePlayers { exported: Some(exported), .. }) = self.phones.remove(device) {
            exported.close().await;
        }
    }
}

fn post_all<'a>(sessions: impl Iterator<Item = &'a SessionHandle>, message: &Message) {
    for session in sessions {
        if !session.post(message.clone()) {
            log::info!("a session's queue is full; dropped a {}", message.kind());
        }
    }
}
