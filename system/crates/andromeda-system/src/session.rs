//! `andromeda-system --session` (docs/system-health.md §1): runs the session-scope rules for one user. It reads the
//! state from the system part, places each episode's holds there (`Knobs.Hold`), alerts through
//! org.freedesktop.Notifications, and serves the merged explain log on the session bus.

use std::collections::HashMap;
use std::path::PathBuf;
use std::sync::Arc;
use std::time::Instant;

use anyhow::{Context, Result};
use futures_util::StreamExt;
use tokio::sync::Mutex;
use zbus::object_server::SignalEmitter;
use zbus::{Connection, MatchRule, MessageStream, OwnedVariant, Variant, interface};

use crate::bus::{self, Remote};
use crate::engine::{Changes, Engine};
use crate::explain::{self, Entry, Log};
use crate::holds::{Knob, Merge};
use crate::policy::now_ms;
use crate::rules::{self, Alert, Scope};
use crate::state::{Fields, Snapshot};
use crate::{NAME, knobs, power};

const BATTERY: &str = "/org/andromeda/System1/Battery/DisplayDevice";
const NOTIFICATIONS: &str = "org.freedesktop.Notifications";
const NOTIFICATIONS_PATH: &str = "/org/freedesktop/Notifications";

/// Alert strings until the i18n catalog reaches this process: key -> (summary, body).
fn alert_text(key: &str) -> (&'static str, &'static str) {
    match key {
        "low_power_plan" => ("Low Power Mode is on", "The battery is low. The desktop uses less power until you plug in."),
        "battery_critical" => ("Battery critically low", "Plug in now. The computer will soon shut down."),
        _ => ("The desktop changed a setting", "Open Settings > Battery and Power to see why."),
    }
}

struct Spec {
    holds: Vec<rules::Hold>,
    alert: Option<Alert>,
}

struct Session {
    engine: Engine,
    log: Log,
    specs: HashMap<String, Spec>,
    /// Open episodes: episode -> (rule id, the system hold ids).
    held: HashMap<u64, (String, Vec<String>)>,
    /// Notification id -> episode.
    alerts: HashMap<u32, u64>,
    state: Snapshot,
}

type Shared = Arc<Mutex<Session>>;

fn variant(value: &toml::Value) -> Option<Variant<'static>> {
    Some(match value {
        toml::Value::String(s) => s.clone().into(),
        toml::Value::Integer(i) => i32::try_from(*i).ok()?.into(),
        toml::Value::Float(f) => (*f).into(),
        toml::Value::Boolean(b) => (*b).into(),
        toml::Value::Array(items) => {
            items.iter().map(|i| i.as_str().map(str::to_owned)).collect::<Option<Vec<_>>>()?.into()
        }
        _ => return None,
    })
}

impl Session {
    async fn place(&self, system: &Connection, rule: &str) -> Vec<String> {
        let mut ids = Vec::new();
        for hold in self.specs.get(rule).map(|s| s.holds.as_slice()).unwrap_or_default() {
            let Some(value) = variant(&hold.value) else { continue };
            let reply = system
                .call_method(Some(NAME), knobs::PATH, Some("org.andromeda.System1.Knobs"), "Hold", &(
                    hold.knob.as_str(),
                    value,
                    rule,
                ))
                .await;
            match reply.and_then(|r| r.body().deserialize::<String>()) {
                Ok(id) => ids.push(id),
                Err(e) => log::warn!("session: {rule} could not hold {}: {e}", hold.knob),
            }
        }
        ids
    }

    async fn alert(&mut self, session: &Connection, entry: &Entry) {
        let Some(alert) = self.specs.get(&entry.rule).and_then(|s| s.alert.clone()) else { return };
        let (summary, body) = alert_text(&alert.text);
        let actions: Vec<&str> = match alert.action.as_deref() {
            Some("undo") => vec!["undo", "Undo"],
            Some("open") => vec!["default", "Open"],
            _ => Vec::new(),
        };
        let urgency: u8 = match alert.urgency.as_str() {
            "critical" => 2,
            "low" => 0,
            _ => 1,
        };
        let mut hints: HashMap<&str, Variant<'_>> = HashMap::new();
        hints.insert("urgency", urgency.into());
        hints.insert("x-andromeda-rule", entry.rule.as_str().into());
        hints.insert("x-andromeda-episode", entry.id.as_str().into());
        let reply = session
            .call_method(Some(NOTIFICATIONS), NOTIFICATIONS_PATH, Some(NOTIFICATIONS), "Notify", &(
                "Andromeda",
                0u32,
                "battery-caution-symbolic",
                summary,
                body,
                actions,
                hints,
                -1i32,
            ))
            .await;
        match reply.and_then(|r| r.body().deserialize::<u32>()) {
            Ok(id) => {
                self.alerts.insert(id, entry.episode);
            }
            Err(e) => log::warn!("session: alert for {}: {e}", entry.rule),
        }
    }

    async fn commit(&mut self, system: &Connection, session: &Connection, changes: Changes) {
        for entry in changes.entries {
            if let Err(e) = self.log.append(&entry) {
                log::warn!("explain log: {e}");
            }
            if let Ok(emitter) = SignalEmitter::new(session, explain::PATH) {
                let _ = explain::Explain::entry(&emitter, entry.to_dict()).await;
            }
            if entry.event == "started" {
                let ids = self.place(system, &entry.rule).await;
                self.held.insert(entry.episode, (entry.rule.clone(), ids));
                self.alert(session, &entry).await;
                continue;
            }
            for id in self.held.remove(&entry.episode).map(|(_, ids)| ids).unwrap_or_default() {
                // The hold may already be gone (a user Set ended it, or the system part restarted).
                let _ = system
                    .call_method(Some(NAME), knobs::PATH, Some("org.andromeda.System1.Knobs"), "Release", &(id,))
                    .await;
            }
            let ended: Vec<u32> = self.alerts.iter().filter(|(_, e)| **e == entry.episode).map(|(n, _)| *n).collect();
            for note in ended {
                self.alerts.remove(&note);
                let _ = session
                    .call_method(Some(NOTIFICATIONS), NOTIFICATIONS_PATH, Some(NOTIFICATIONS), "CloseNotification", &(
                        note,
                    ))
                    .await;
            }
        }
    }

    async fn undo(&mut self, system: &Connection, session: &Connection, episode: u64) -> bool {
        let Some(changes) = self.engine.undo(episode, now_ms()) else { return false };
        self.commit(system, session, changes).await;
        true
    }
}

/// The session's explain log merged with the system part's.
struct SessionExplain {
    shared: Shared,
    system: Connection,
}

#[interface(name = "org.andromeda.System1.Explain")]
impl SessionExplain {
    async fn list(&self, since_usec: u64) -> Vec<HashMap<String, OwnedVariant>> {
        let system: Vec<HashMap<String, OwnedVariant>> = match self
            .system
            .call_method(Some(NAME), explain::PATH, Some("org.andromeda.System1.Explain"), "List", &(since_usec,))
            .await
        {
            Ok(reply) => reply.body().deserialize().unwrap_or_default(),
            Err(_) => Vec::new(),
        };
        let own = self.shared.lock().await.log.since(since_usec / 1000);
        let mut all: Vec<(u64, HashMap<String, OwnedVariant>)> = own.iter().map(|e| (e.time_ms * 1000, e.to_dict())).collect();
        for entry in system {
            let time = entry.get("time_usec").and_then(|v| u64::try_from(v).ok()).unwrap_or(0);
            all.push((time, entry));
        }
        all.sort_by_key(|(time, _)| *time);
        all.into_iter().map(|(_, e)| e).collect()
    }

    /// Undoes a session entry here, or passes a system entry to the system part.
    async fn undo(&self, entry: &str, #[zbus(connection)] session: &Connection) -> zbus::fdo::Result<()> {
        let mut inner = self.shared.lock().await;
        if let Some(episode) = inner.log.since(0).into_iter().find(|e| e.id == entry).map(|e| e.episode) {
            return if inner.undo(&self.system, session, episode).await {
                Ok(())
            } else {
                Err(zbus::fdo::Error::Failed("the episode already ended".into()))
            };
        }
        drop(inner);
        self.system
            .call_method(Some(NAME), explain::PATH, Some("org.andromeda.System1.Explain"), "Undo", &(entry,))
            .await
            .map(|_| ())
            .map_err(|e| zbus::fdo::Error::Failed(e.to_string()))
    }
}

fn rules_dir() -> PathBuf {
    std::env::var_os("ANDROMEDA_RULES_DIR").map_or_else(|| "/usr/share/andromeda/system/rules".into(), PathBuf::from)
}

fn load_rules() -> (Engine, HashMap<String, Spec>) {
    let mut rules = Vec::new();
    let mut paths: Vec<PathBuf> = std::fs::read_dir(rules_dir())
        .map(|dir| dir.filter_map(|e| e.ok().map(|e| e.path())).collect())
        .unwrap_or_default();
    paths.retain(|p| p.extension().is_some_and(|e| e == "toml"));
    paths.sort();
    for path in paths {
        let text = std::fs::read_to_string(&path).unwrap_or_default();
        match rules::load(&text, &Fields) {
            Ok(loaded) => {
                for rule in loaded {
                    match rule {
                        Ok(rule) if rule.scope == Scope::Session => rules.push(rule),
                        Ok(_) => {}
                        Err((id, why)) => log::warn!("rule {id} is off: {why}"),
                    }
                }
            }
            Err(e) => log::warn!("{}: {e}", path.display()),
        }
    }
    let config = std::env::var_os("XDG_CONFIG_HOME")
        .map(PathBuf::from)
        .or_else(|| std::env::var_os("HOME").map(|h| PathBuf::from(h).join(".config")));
    if let Some(text) = config.and_then(|c| std::fs::read_to_string(c.join("andromeda/system-rules.toml")).ok()) {
        for warning in rules::apply_settings(&mut rules, &text) {
            log::warn!("{warning}");
        }
    }
    let specs = rules.iter().map(|r| (r.id.clone(), Spec { holds: r.holds.clone(), alert: r.alert.clone() })).collect();
    let (engine, refused) = Engine::new(vec![Knob::new("power.profile", Merge::LowestPower)], rules, now_ms());
    for (id, why) in refused {
        log::warn!("rule {id} is off: {why}");
    }
    (engine, specs)
}

pub async fn run() -> Result<()> {
    let system = Connection::system().await?;
    let (engine, specs) = load_rules();
    let state_dir = std::env::var_os("XDG_STATE_HOME")
        .map(PathBuf::from)
        .or_else(|| std::env::var_os("HOME").map(|h| PathBuf::from(h).join(".local/state")))
        .context("no XDG_STATE_HOME or HOME")?;
    let shared = Arc::new(Mutex::new(Session {
        engine,
        log: Log::new(state_dir.join("andromeda/system/explain.jsonl")),
        specs,
        held: HashMap::new(),
        alerts: HashMap::new(),
        state: Snapshot::default(),
    }));
    let session = zbus::connection::Builder::session()
        .serve_at(explain::PATH, SessionExplain { shared: shared.clone(), system: system.clone() })
        .name(NAME)
        .build()
        .await?;
    let _ = sd_notify::notify(&[sd_notify::NotifyState::Ready]);
    log::info!("serving {NAME} on the session bus");
    loop {
        if let Err(e) = follow(&system, &session, &shared).await {
            log::warn!("session: {e:#}");
        }
        bus::appeared(&system, NAME).await?;
    }
}

/// One life of the system part: follows its state, steps the rules, and re-places the holds of open episodes.
async fn follow(system: &Connection, session: &Connection, shared: &Shared) -> Result<()> {
    let gone = bus::vanished(system, NAME);
    let mut power = Remote::new(system, NAME, power::PATH.to_owned(), "org.andromeda.System1.Power").await?;
    let mut battery = Remote::new(system, NAME, BATTERY.to_owned(), "org.andromeda.System1.Battery").await.ok();
    let mut released = MessageStream::for_match_rule(
        MatchRule::builder()
            .msg_type(zbus::message::Type::Signal)
            .sender(NAME)
            .interface("org.andromeda.System1.Knobs")
            .member("Released")
            .build()?,
        system,
        None,
    )
    .await?;
    let mut actions = MessageStream::for_match_rule(
        MatchRule::builder()
            .msg_type(zbus::message::Type::Signal)
            .interface(NOTIFICATIONS)
            .member("ActionInvoked")
            .build()?,
        session,
        None,
    )
    .await?;
    let me = system.unique_name().map(ToString::to_string).unwrap_or_default();
    {
        let mut inner = shared.lock().await;
        inner.state.set_power(&power.props);
        inner.state.set_battery(battery.as_ref().map(|b| &b.props));
        let open: Vec<(u64, String)> = inner.held.iter().map(|(e, (rule, _))| (*e, rule.clone())).collect();
        for (episode, rule) in open {
            let ids = inner.place(system, &rule).await;
            inner.held.insert(episode, (rule, ids));
        }
    }
    tokio::pin!(gone);
    loop {
        {
            let mut inner = shared.lock().await;
            let state = inner.state.clone();
            let changes = inner.engine.step(&state, Instant::now(), now_ms());
            inner.commit(system, session, changes).await;
        }
        let deadline = shared.lock().await.engine.deadline();
        let sleep = async {
            match deadline {
                Some(at) => tokio::time::sleep_until(at.into()).await,
                None => std::future::pending().await,
            }
        };
        let battery_next = async {
            match battery.as_mut() {
                Some(b) => b.next().await,
                None => std::future::pending().await,
            }
        };
        tokio::select! {
            result = &mut gone => return result,
            change = power.next() => {
                change.context("power stream ended")??;
                shared.lock().await.state.set_power(&power.props);
            }
            change = battery_next => {
                if !matches!(change, Some(Ok(()))) {
                    battery = None;
                }
                let mut inner = shared.lock().await;
                inner.state.set_battery(battery.as_ref().map(|b| &b.props));
            }
            message = released.next() => {
                let message = message.context("Released stream ended")??;
                let Ok((id, owner)) = message.body().deserialize::<(String, String)>() else { continue };
                if owner != me {
                    continue;
                }
                let mut inner = shared.lock().await;
                let episode = inner.held.iter().find(|(_, (_, ids))| ids.contains(&id)).map(|(e, _)| *e);
                if let Some(episode) = episode {
                    inner.undo(system, session, episode).await;
                }
            }
            message = actions.next() => {
                let message = message.context("ActionInvoked stream ended")??;
                let Ok((note, action)) = message.body().deserialize::<(u32, String)>() else { continue };
                let mut inner = shared.lock().await;
                if let (Some(episode), "undo") = (inner.alerts.get(&note).copied(), action.as_str()) {
                    inner.undo(system, session, episode).await;
                }
            }
            () = sleep => {}
        }
    }
}
