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
use crate::expr::Value;
use crate::state::{Fields, Snapshot};
use crate::{NAME, knobs, power};

const BATTERY: &str = "/org/andromeda/System1/Battery/DisplayDevice";
const RULES: &str = "/org/andromeda/System1/Rules";
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

fn settings_path() -> Option<PathBuf> {
    std::env::var_os("XDG_CONFIG_HOME")
        .map(PathBuf::from)
        .or_else(|| std::env::var_os("HOME").map(|h| PathBuf::from(h).join(".config")))
        .map(|c| c.join("andromeda/system-rules.toml"))
}

/// The rules, their alert specs, and the rules that failed to load with why.
fn load_rules() -> (Engine, HashMap<String, Spec>, Vec<(String, String)>) {
    let mut rules = Vec::new();
    let mut broken = Vec::new();
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
                        Err((id, why)) => {
                            log::warn!("rule {id} is off: {why}");
                            broken.push((id, why));
                        }
                    }
                }
            }
            Err(e) => log::warn!("{}: {e}", path.display()),
        }
    }
    let specs = rules.iter().map(|r| (r.id.clone(), Spec { holds: r.holds.clone(), alert: r.alert.clone() })).collect();
    let (mut engine, refused) = Engine::new(vec![Knob::new("power.profile", Merge::LowestPower)], rules, now_ms());
    for (id, why) in refused {
        log::warn!("rule {id} is off: {why}");
        broken.push((id, why));
    }
    let text = settings_path().and_then(|p| std::fs::read_to_string(p).ok()).unwrap_or_default();
    match rules::parse_settings(&text) {
        Ok(settings) => {
            for (id, setting) in settings {
                if let Err(e) = engine.configure(&id, setting.enabled, &setting.params, now_ms()) {
                    log::warn!("settings: {e}");
                }
            }
        }
        Err(e) => log::warn!("{e}"),
    }
    (engine, specs, broken)
}

/// `/org/andromeda/System1/Rules` on the session bus: the rules as Settings rows.
struct RulesObject {
    shared: Shared,
    system: Connection,
    broken: Vec<(String, String)>,
}

fn param_variant(value: &Value) -> Option<OwnedVariant> {
    let variant: Variant<'_> = match value {
        Value::Num(n) => (*n).into(),
        Value::Bool(b) => (*b).into(),
        Value::Str(s) => s.as_str().into(),
    };
    OwnedVariant::try_from(variant).ok()
}

#[interface(name = "org.andromeda.System1.Rules")]
impl RulesObject {
    /// Each rule: id, on, why it cannot run ("" when it loaded), and its params.
    async fn list(&self) -> Vec<(String, bool, String, HashMap<String, OwnedVariant>)> {
        let inner = self.shared.lock().await;
        let mut out: Vec<_> = inner
            .engine
            .rules()
            .map(|(rule, enabled)| {
                let params = rule.params.iter().filter_map(|(k, v)| Some((k.clone(), param_variant(v)?))).collect();
                (rule.id.clone(), enabled, String::new(), params)
            })
            .collect();
        out.extend(self.broken.iter().map(|(id, why)| (id.clone(), false, why.clone(), HashMap::new())));
        out
    }

    /// Turns a rule on or off and sets its params (numbers as d, switches as b); stored in
    /// ~/.config/andromeda/system-rules.toml.
    async fn set(
        &self,
        id: &str,
        enabled: bool,
        params: HashMap<String, OwnedVariant>,
        #[zbus(connection)] session: &Connection,
    ) -> zbus::fdo::Result<()> {
        let params: Vec<(String, Value)> = params
            .into_iter()
            .map(|(k, v)| {
                let value = f64::try_from(&v).map(Value::Num).or_else(|_| bool::try_from(&v).map(Value::Bool));
                value.map(|value| (k.clone(), value)).map_err(|_| zbus::fdo::Error::InvalidArgs(format!("{k}: d or b")))
            })
            .collect::<zbus::fdo::Result<_>>()?;
        let mut inner = self.shared.lock().await;
        let changes = inner.engine.configure(id, enabled, &params, now_ms()).map_err(zbus::fdo::Error::InvalidArgs)?;
        let (rule, _) = inner.engine.rules().find(|(r, _)| r.id == id).ok_or_else(|| zbus::fdo::Error::Failed(id.into()))?;
        let mut entry = toml::Table::new();
        entry.insert("enabled".into(), toml::Value::Boolean(enabled));
        for (k, v) in &rule.params {
            match v {
                Value::Num(n) => entry.insert(k.clone(), toml::Value::Float(*n)),
                Value::Bool(b) => entry.insert(k.clone(), toml::Value::Boolean(*b)),
                Value::Str(_) => None,
            };
        }
        save_setting(id, entry).map_err(|e| zbus::fdo::Error::Failed(format!("{e:#}")))?;
        inner.commit(&self.system, session, changes).await;
        Ok(())
    }
}

fn save_setting(id: &str, entry: toml::Table) -> Result<()> {
    let path = settings_path().context("no XDG_CONFIG_HOME or HOME")?;
    let mut table: toml::Table =
        std::fs::read_to_string(&path).ok().and_then(|t| toml::from_str(&t).ok()).unwrap_or_default();
    table.insert(id.to_owned(), toml::Value::Table(entry));
    if let Some(dir) = path.parent() {
        std::fs::create_dir_all(dir)?;
    }
    let tmp = path.with_extension("tmp");
    std::fs::write(&tmp, toml::to_string(&table)?)?;
    std::fs::rename(tmp, path)?;
    Ok(())
}

pub async fn run() -> Result<()> {
    let system = Connection::system().await?;
    let (engine, specs, broken) = load_rules();
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
        .serve_at(RULES, RulesObject { shared: shared.clone(), system: system.clone(), broken })
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
