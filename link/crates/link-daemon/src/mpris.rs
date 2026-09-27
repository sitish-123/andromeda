//! A phone's player exported as an MPRIS player, `org.mpris.MediaPlayer2.umbriel_link_<device id>`, on its own
//! session-bus connection: the shell's media widget and lock screen see it like any other player.

#![expect(clippy::unused_self, reason = "MPRIS requires these members, and zbus serves only methods")]

use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::time::Instant;

use link_core::identity::DeviceId;
use link_core::proto::message::{MediaCommand, MediaCommandKind, MediaPlayer, Message, PlaybackState};
use ring::digest;
use zbus::object_server::SignalEmitter;
use zbus::zvariant::{ObjectPath, Value};

use crate::hub::HubHandle;

const PATH: &str = "/org/mpris/MediaPlayer2";
/// The prefix of every name this daemon exports, which its own MPRIS client skips.
pub const NAME_PREFIX: &str = "org.mpris.MediaPlayer2.umbriel_link_";

struct Root {
    identity: String,
}

#[zbus::interface(name = "org.mpris.MediaPlayer2")]
impl Root {
    fn raise(&self) {}

    fn quit(&self) {}

    #[zbus(property)]
    fn can_quit(&self) -> bool {
        false
    }

    #[zbus(property)]
    fn can_raise(&self) -> bool {
        false
    }

    #[zbus(property)]
    fn has_track_list(&self) -> bool {
        false
    }

    #[zbus(property)]
    fn identity(&self) -> String {
        self.identity.clone()
    }

    #[zbus(property)]
    fn supported_uri_schemes(&self) -> Vec<String> {
        Vec::new()
    }

    #[zbus(property)]
    fn supported_mime_types(&self) -> Vec<String> {
        Vec::new()
    }
}

struct Playback {
    hub: HubHandle,
    device: DeviceId,
    player: MediaPlayer,
    /// When `player.position_ms` was true.
    at: Instant,
    art_url: Option<String>,
    track: u64,
}

impl Playback {
    fn can(&self, command: MediaCommandKind) -> bool {
        self.player.can.contains(&command)
    }

    fn position_ms(&self) -> u64 {
        let elapsed = if self.player.state == PlaybackState::Playing {
            u64::try_from(self.at.elapsed().as_millis()).unwrap_or(u64::MAX)
        } else {
            0
        };
        let position = self.player.position_ms.saturating_add(elapsed);
        self.player.length_ms.map_or(position, |length| position.min(length))
    }

    async fn command(&self, command: MediaCommandKind, value: Option<u64>) -> zbus::fdo::Result<()> {
        if !self.can(command) {
            return Err(zbus::fdo::Error::NotSupported(format!("the phone's player cannot {command:?}")));
        }
        let Some(session) = self.hub.session(self.device.clone()).await else {
            return Err(zbus::fdo::Error::Failed("the phone is not connected".to_owned()));
        };
        let message = Message::MediaCommand(MediaCommand { player: self.player.player.clone(), command, value });
        session.send(message).await.map_err(|error| zbus::fdo::Error::Failed(error.to_string()))
    }

    async fn seek_to(&self, target_us: i64) -> zbus::fdo::Result<()> {
        let target = u64::try_from(target_us / 1000).unwrap_or(0);
        let target = self.player.length_ms.map_or(target, |length| target.min(length));
        self.command(MediaCommandKind::Seek, Some(target)).await
    }
}

#[zbus::interface(name = "org.mpris.MediaPlayer2.Player")]
impl Playback {
    async fn next(&self) -> zbus::fdo::Result<()> {
        self.command(MediaCommandKind::Next, None).await
    }

    async fn previous(&self) -> zbus::fdo::Result<()> {
        self.command(MediaCommandKind::Previous, None).await
    }

    async fn pause(&self) -> zbus::fdo::Result<()> {
        self.command(MediaCommandKind::Pause, None).await
    }

    async fn play_pause(&self) -> zbus::fdo::Result<()> {
        self.command(MediaCommandKind::PlayPause, None).await
    }

    async fn stop(&self) -> zbus::fdo::Result<()> {
        self.command(MediaCommandKind::Pause, None).await
    }

    async fn play(&self) -> zbus::fdo::Result<()> {
        self.command(MediaCommandKind::Play, None).await
    }

    async fn seek(&self, offset: i64) -> zbus::fdo::Result<()> {
        let position = i64::try_from(self.position_ms()).unwrap_or(i64::MAX).saturating_mul(1000);
        self.seek_to(position.saturating_add(offset).max(0)).await
    }

    async fn set_position(&self, track_id: ObjectPath<'_>, position: i64) -> zbus::fdo::Result<()> {
        if track_id.as_str() != self.track_path() {
            return Ok(());
        }
        self.seek_to(position).await
    }

    fn open_uri(&self, uri: &str) {
        log::debug!("OpenUri({uri}) on a phone player: not supported");
    }

    #[zbus(signal)]
    async fn seeked(emitter: &SignalEmitter<'_>, position: i64) -> zbus::Result<()>;

    #[zbus(property)]
    fn playback_status(&self) -> String {
        match self.player.state {
            PlaybackState::Playing => "Playing",
            PlaybackState::Paused => "Paused",
            PlaybackState::Stopped => "Stopped",
        }
        .to_owned()
    }

    #[zbus(property)]
    fn metadata(&self) -> HashMap<String, Value<'static>> {
        let mut metadata = HashMap::new();
        let track = ObjectPath::try_from(self.track_path()).map(ObjectPath::into_owned);
        if let Ok(track) = track {
            metadata.insert("mpris:trackid".to_owned(), Value::from(track));
        }
        metadata.insert("xesam:title".to_owned(), Value::from(self.player.title.clone()));
        if !self.player.artist.is_empty() {
            metadata.insert("xesam:artist".to_owned(), Value::from(vec![self.player.artist.clone()]));
        }
        if !self.player.album.is_empty() {
            metadata.insert("xesam:album".to_owned(), Value::from(self.player.album.clone()));
        }
        if let Some(length) = self.player.length_ms {
            metadata.insert("mpris:length".to_owned(), Value::from(i64::try_from(length).unwrap_or(0) * 1000));
        }
        if let Some(url) = &self.art_url {
            metadata.insert("mpris:artUrl".to_owned(), Value::from(url.clone()));
        }
        metadata
    }

    #[zbus(property(emits_changed_signal = "false"))]
    fn position(&self) -> i64 {
        i64::try_from(self.position_ms()).unwrap_or(i64::MAX).saturating_mul(1000)
    }

    #[zbus(property)]
    fn volume(&self) -> f64 {
        self.player.volume.map_or(1.0, |volume| f64::from(volume) / 100.0)
    }

    #[zbus(property)]
    async fn set_volume(&mut self, volume: f64) -> zbus::Result<()> {
        // Rounded and clamped into 0..=100 first, so the cast cannot truncate.
        #[expect(clippy::cast_possible_truncation, clippy::cast_sign_loss, reason = "clamped to 0..=100")]
        let percent = (volume.clamp(0.0, 1.0) * 100.0).round() as u64;
        self.command(MediaCommandKind::Volume, Some(percent)).await.map_err(zbus::Error::from)
    }

    #[zbus(property)]
    fn rate(&self) -> f64 {
        1.0
    }

    #[zbus(property)]
    fn minimum_rate(&self) -> f64 {
        1.0
    }

    #[zbus(property)]
    fn maximum_rate(&self) -> f64 {
        1.0
    }

    #[zbus(property)]
    fn can_go_next(&self) -> bool {
        self.can(MediaCommandKind::Next)
    }

    #[zbus(property)]
    fn can_go_previous(&self) -> bool {
        self.can(MediaCommandKind::Previous)
    }

    #[zbus(property)]
    fn can_play(&self) -> bool {
        self.can(MediaCommandKind::Play) || self.can(MediaCommandKind::PlayPause)
    }

    #[zbus(property)]
    fn can_pause(&self) -> bool {
        self.can(MediaCommandKind::Pause) || self.can(MediaCommandKind::PlayPause)
    }

    #[zbus(property)]
    fn can_seek(&self) -> bool {
        self.can(MediaCommandKind::Seek)
    }

    #[zbus(property)]
    fn can_control(&self) -> bool {
        true
    }
}

impl Playback {
    fn track_path(&self) -> String {
        format!("/org/umbriel/Link1/track/{}", self.track)
    }
}

/// One phone's exported player. Dropping it releases the bus name; [`Exported::close`] also removes its artwork.
pub struct Exported {
    connection: zbus::Connection,
    name: String,
    art_dir: PathBuf,
    art_file: Option<PathBuf>,
}

impl Exported {
    pub async fn start(
        hub: HubHandle,
        device: DeviceId,
        device_name: &str,
        player: &MediaPlayer,
        art_dir: &Path,
    ) -> anyhow::Result<Self> {
        let identity = format!("{} on {device_name}", player.name);
        let (art_file, art_url) = write_art(art_dir, &device, player);
        let state =
            Playback { hub, device: device.clone(), player: player.clone(), at: Instant::now(), art_url, track: 0 };
        let name = format!("{NAME_PREFIX}{device}");
        let connection = zbus::connection::Builder::session()?
            .name(name.clone())?
            .serve_at(PATH, Root { identity })?
            .serve_at(PATH, state)?
            .build()
            .await?;
        Ok(Self { connection, name, art_dir: art_dir.to_owned(), art_file })
    }

    pub async fn update(&mut self, device: &DeviceId, player: &MediaPlayer) -> anyhow::Result<()> {
        let interface = self.connection.object_server().interface::<_, Playback>(PATH).await?;
        let mut state = interface.get_mut().await;
        let old = std::mem::replace(&mut state.player, player.clone());
        let expected = old.position_ms.saturating_add(u64::try_from(state.at.elapsed().as_millis()).unwrap_or(0));
        state.at = Instant::now();
        let emitter = interface.signal_emitter();
        let track_changed = (old.title != player.title || old.artist != player.artist) || old.artwork != player.artwork;
        if track_changed {
            state.track += 1;
            let (art_file, art_url) = write_art(&self.art_dir, device, player);
            if let Some(old_file) = std::mem::replace(&mut self.art_file, art_file)
                && Some(&old_file) != self.art_file.as_ref()
            {
                drop(std::fs::remove_file(old_file));
            }
            state.art_url = art_url;
        }
        if track_changed || old.album != player.album || old.length_ms != player.length_ms {
            state.metadata_changed(emitter).await?;
        }
        if old.state != player.state {
            state.playback_status_changed(emitter).await?;
        }
        if old.volume != player.volume {
            state.volume_changed(emitter).await?;
        }
        if old.can != player.can {
            state.can_go_next_changed(emitter).await?;
            state.can_go_previous_changed(emitter).await?;
            state.can_play_changed(emitter).await?;
            state.can_pause_changed(emitter).await?;
            state.can_seek_changed(emitter).await?;
        }
        let playing = old.state == PlaybackState::Playing;
        let expected = if playing { expected } else { old.position_ms };
        if !track_changed && expected.abs_diff(player.position_ms) > SEEK_TOLERANCE_MS {
            let position = i64::try_from(player.position_ms).unwrap_or(i64::MAX).saturating_mul(1000);
            Playback::seeked(emitter, position).await?;
        }
        Ok(())
    }

    pub async fn close(self) {
        if let Some(file) = &self.art_file {
            drop(std::fs::remove_file(file));
        }
        if let Err(error) = self.connection.release_name(self.name.as_str()).await {
            log::warn!("releasing {}: {error}", self.name);
        }
    }
}

/// A report further than this from where the position should be is a seek.
const SEEK_TOLERANCE_MS: u64 = 2000;

/// Writes the artwork under a name derived from its bytes, so a changed image gets a new URL and caches refresh.
fn write_art(dir: &Path, device: &DeviceId, player: &MediaPlayer) -> (Option<PathBuf>, Option<String>) {
    let Some(art) = &player.artwork else { return (None, None) };
    let extension = if art.starts_with(b"\x89PNG") { "png" } else { "jpg" };
    let hash = hex_prefix(digest::digest(&digest::SHA256, art).as_ref());
    let file = dir.join(format!("{device}-{hash}.{extension}"));
    let written = std::fs::create_dir_all(dir).and_then(|()| std::fs::write(&file, art));
    match written {
        Ok(()) => {
            let url = format!("file://{}", file.display());
            (Some(file), Some(url))
        }
        Err(error) => {
            log::warn!("{device}: writing artwork: {error}");
            (None, None)
        }
    }
}

fn hex_prefix(bytes: &[u8]) -> String {
    hex::encode(&bytes[..8])
}
