//! What the headless phone mirrors while it holds a session, driven by stdin commands, and re-sent after every
//! connect as the app does.

use std::collections::BTreeMap;

use anyhow::{Context, anyhow, bail};
use link_core::client::{Client, ClientEvent};
use link_core::identity::DeviceId;
use link_core::proto::message::{
    Call, CallActionKind, CallState, MediaCommand, MediaCommandKind, MediaGone, MediaPlayer, Message,
    NotificationButton, NotificationPosted, NotificationRemoved, PlaybackState, Ring, Ringing,
};
use serde_json::{Value, json};

pub struct Held {
    client: Client,
    desktop: DeviceId,
    notifications: BTreeMap<String, NotificationPosted>,
    /// The phone's own players, which act on the desktop's commands as a real player would.
    players: BTreeMap<String, MediaPlayer>,
    ringing: bool,
}

impl Held {
    pub fn new(client: Client, desktop: DeviceId) -> Self {
        Self { client, desktop, notifications: BTreeMap::new(), players: BTreeMap::new(), ringing: false }
    }

    /// `notify <json>` posts or replaces a notification (see [`notification`]); `unnotify <id>` removes it;
    /// `media <json>` plays or updates a player (a `media-player` body, plus `artwork_file`); `media-gone <player>`
    /// stops it; `media-command <json>` commands a desktop player (a `media-command` body); `reconnect` drops the
    /// session and dials again; `ring <on|off>` rings the desktop or stops it; `ringing <on|off>` reports this phone's
    /// own ringing, as stopping it on the phone does; `call <json>` reports a call (a `call` body).
    pub async fn command(&mut self, line: &str) -> Value {
        let (verb, rest) = line.split_once(' ').unwrap_or((line, ""));
        let result = match verb {
            "notify" => self.notify(rest).await,
            "unnotify" => self.unnotify(rest.trim()).await,
            "reconnect" => self.reconnect().await,
            "media" => self.media(rest).await,
            "media-gone" => self.media_gone(rest.trim()).await,
            "media-command" => self.media_command(rest).await,
            "ring" => self.send_switch(rest, |on| Message::Ring(Ring { on })).await,
            "call" => self.call(rest).await,
            "ringing" => {
                self.ringing = rest.trim() == "on";
                self.send_switch(rest, |on| Message::Ringing(Ringing { on })).await
            }
            _ => Err(anyhow!("unknown command")),
        };
        result.unwrap_or_else(|error| json!({ "event": "command-failed", "command": verb, "error": error.to_string() }))
    }

    pub async fn on_event(&mut self, event: &ClientEvent) {
        match event {
            ClientEvent::Connected { .. } => {
                for posted in self.notifications.values() {
                    self.broadcast(Message::NotificationPosted(posted.clone())).await;
                }
                for player in self.players.values() {
                    self.broadcast(Message::MediaPlayer(player.clone())).await;
                }
            }
            ClientEvent::Message { message: Message::MediaCommand(command), .. } => self.obey(command).await,
            // As Android does: a declined call ends, and the call state goes idle.
            ClientEvent::Message { message: Message::CallAction(action), .. }
                if action.action == CallActionKind::Decline =>
            {
                let idle = Message::Call(Call { state: CallState::Idle, number: None, name: None });
                self.broadcast(idle).await;
            }
            // As the app does: ring or stop, then report the state.
            ClientEvent::Message { from, message: Message::Ring(ring) } => {
                self.ringing = ring.on;
                let report = Message::Ringing(Ringing { on: self.ringing });
                if let Err(error) = self.client.send(from.clone(), report).await {
                    log::warn!("reporting the ring: {error}");
                }
            }
            // As Android does: cancelling the notification removes it, and the listener reports the removal.
            ClientEvent::Message { message: Message::NotificationDismiss(dismiss), .. }
                if self.notifications.remove(&dismiss.id).is_some() =>
            {
                self.broadcast(Message::NotificationRemoved(NotificationRemoved { id: dismiss.id.clone() })).await;
            }
            _ => {}
        }
    }

    async fn notify(&mut self, json: &str) -> anyhow::Result<Value> {
        let posted = notification(&serde_json::from_str(json).context("notify takes a json object")?)?;
        let id = posted.id.clone();
        let sent = self.client.broadcast(Message::NotificationPosted(posted.clone())).await?;
        self.notifications.insert(id.clone(), posted);
        Ok(json!({ "event": "notified", "id": id, "desktops": sent }))
    }

    async fn unnotify(&mut self, id: &str) -> anyhow::Result<Value> {
        if self.notifications.remove(id).is_none() {
            bail!("no notification {id}");
        }
        let sent =
            self.client.broadcast(Message::NotificationRemoved(NotificationRemoved { id: id.to_owned() })).await?;
        Ok(json!({ "event": "unnotified", "id": id, "desktops": sent }))
    }

    async fn media(&mut self, json: &str) -> anyhow::Result<Value> {
        let mut value: Value = serde_json::from_str(json).context("media takes a json object")?;
        let artwork = match value.as_object_mut().and_then(|object| object.remove("artwork_file")) {
            Some(Value::String(path)) => Some(std::fs::read(&path).with_context(|| format!("reading {path}"))?),
            _ => None,
        };
        let mut player: MediaPlayer = serde_json::from_value(value).context("not a media-player body")?;
        player.artwork = artwork;
        let sent = self.client.broadcast(Message::MediaPlayer(player.clone())).await?;
        let id = player.player.clone();
        self.players.insert(id.clone(), player);
        Ok(json!({ "event": "playing", "player": id, "desktops": sent }))
    }

    async fn media_gone(&mut self, player: &str) -> anyhow::Result<Value> {
        self.players.remove(player).with_context(|| format!("no player {player}"))?;
        let sent = self.client.broadcast(Message::MediaGone(MediaGone { player: player.to_owned() })).await?;
        Ok(json!({ "event": "media-gone", "player": player, "desktops": sent }))
    }

    async fn media_command(&self, json: &str) -> anyhow::Result<Value> {
        let command: MediaCommand = serde_json::from_str(json).context("not a media-command body")?;
        self.client.send(self.desktop.clone(), Message::MediaCommand(command.clone())).await?;
        Ok(json!({ "event": "media-commanded", "player": command.player }))
    }

    async fn call(&self, json: &str) -> anyhow::Result<Value> {
        let call: Call = serde_json::from_str(json).context("not a call body")?;
        let state = call.state.as_str();
        let sent = self.client.broadcast(Message::Call(call)).await?;
        Ok(json!({ "event": "call-reported", "state": state, "desktops": sent }))
    }

    async fn send_switch(&self, rest: &str, message: impl FnOnce(bool) -> Message) -> anyhow::Result<Value> {
        let on = match rest.trim() {
            "on" => true,
            "off" => false,
            other => bail!("on or off, not {other:?}"),
        };
        let message = message(on);
        let kind = message.kind();
        self.client.send(self.desktop.clone(), message).await?;
        Ok(json!({ "event": "sent", "type": kind, "on": on }))
    }

    /// Plays the desktop's command on its own player and reports the new state, as a phone's player would.
    async fn obey(&mut self, command: &MediaCommand) {
        let Some(player) = self.players.get_mut(&command.player) else { return };
        if !player.can.contains(&command.command) {
            return;
        }
        let value = command.value.unwrap_or(0);
        match command.command {
            MediaCommandKind::Play => player.state = PlaybackState::Playing,
            MediaCommandKind::Pause => player.state = PlaybackState::Paused,
            MediaCommandKind::PlayPause => {
                player.state =
                    if player.state == PlaybackState::Playing { PlaybackState::Paused } else { PlaybackState::Playing };
            }
            MediaCommandKind::Next | MediaCommandKind::Previous => {
                player.title = format!("{} ({:?})", player.title, command.command);
                player.position_ms = 0;
            }
            MediaCommandKind::Seek => player.position_ms = value,
            MediaCommandKind::Volume => player.volume = u8::try_from(value).ok(),
        }
        let update = Message::MediaPlayer(player.clone());
        self.broadcast(update).await;
    }

    /// Drops the session and dials again at once, as the app does when Android's default network changes.
    async fn reconnect(&self) -> anyhow::Result<Value> {
        self.client.set_present(false).await?;
        self.client.set_present(true).await?;
        Ok(json!({ "event": "reconnecting" }))
    }

    async fn broadcast(&self, message: Message) {
        if let Err(error) = self.client.broadcast(message).await {
            log::warn!("re-sending: {error}");
        }
    }
}

/// `{id, app, title, text, icon_file?, actions: [{id, label, reply}]}`, the icon read from a PNG file.
pub fn notification(value: &Value) -> anyhow::Result<NotificationPosted> {
    let text = |key: &str| value[key].as_str().map(str::to_owned).with_context(|| format!("no {key}"));
    let icon = match value["icon_file"].as_str() {
        Some(path) => Some(std::fs::read(path).with_context(|| format!("reading {path}"))?),
        None => None,
    };
    let actions = value["actions"]
        .as_array()
        .map(Vec::as_slice)
        .unwrap_or_default()
        .iter()
        .map(|action| {
            Ok(NotificationButton {
                id: action["id"].as_str().context("an action without id")?.to_owned(),
                label: action["label"].as_str().context("an action without label")?.to_owned(),
                reply: action["reply"].as_bool().unwrap_or(false),
            })
        })
        .collect::<anyhow::Result<_>>()?;
    Ok(NotificationPosted {
        id: text("id")?,
        app: text("app")?,
        title: text("title")?,
        text: text("text")?,
        icon,
        actions,
    })
}

pub fn describe(from: &DeviceId, message: &Message) -> Value {
    match message {
        Message::NotificationAction(action) => json!({
            "event": "notification-action", "desktop": from, "id": action.id, "action": action.action,
            "reply_text": action.reply_text,
        }),
        Message::NotificationDismiss(dismiss) => {
            json!({ "event": "notification-dismiss", "desktop": from, "id": dismiss.id })
        }
        Message::MediaPlayer(player) => json!({
            "event": "media-player", "desktop": from, "player": player.player, "name": player.name,
            "state": player.state, "title": player.title, "artist": player.artist, "position_ms": player.position_ms,
            "volume": player.volume, "can": player.can, "artwork_bytes": player.artwork.as_ref().map(Vec::len),
        }),
        Message::MediaGone(gone) => json!({ "event": "media-gone", "desktop": from, "player": gone.player }),
        Message::Ring(ring) => json!({ "event": "ring", "desktop": from, "on": ring.on }),
        Message::CallAction(action) => json!({ "event": "call-action", "desktop": from, "action": action.action }),
        Message::Ringing(ringing) => json!({ "event": "desktop-ringing", "desktop": from, "on": ringing.on }),
        Message::MediaCommand(command) => json!({
            "event": "media-command", "desktop": from, "player": command.player, "command": command.command,
            "value": command.value,
        }),
        other => json!({ "event": "message", "desktop": from, "type": other.kind() }),
    }
}
