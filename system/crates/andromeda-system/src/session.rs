//! `andromeda-system --session` (docs/system-health.md §1): runs the session-scope rules for one user. It reads the
//! state from the system part, places each episode's holds there (`Knobs.Hold`), alerts through
//! org.freedesktop.Notifications, and serves the merged explain log on the session bus.

use std::collections::HashMap;
use std::path::PathBuf;
use std::sync::Arc;
use std::time::Instant;

use anyhow::{Context, Result};
use futures_util::StreamExt;
use tokio::sync::{Mutex, Notify};
use zbus::object_server::SignalEmitter;
use zbus::{Connection, MatchRule, MessageStream, OwnedVariant, Variant, interface};

use crate::bus::{self, Remote};
use crate::engine::{Changes, Engine};
use crate::explain::{self, Entry, Log};
use crate::expr::Value;
use crate::holds::{Knob, KnobValue, Merge};
use crate::policy::now_ms;
use crate::rules::RuleValues;
use crate::rules::{self, Alert, Scope};
use crate::state::{AppRow, AppValues, Fields, Snapshot};
use crate::{NAME, knobs, memory, nm, power, thermal};

const BATTERY: &str = "/org/andromeda/System1/Battery/DisplayDevice";
const RULES: &str = "/org/andromeda/System1/Rules";
/// Knobs the system part owns; the session part holds them with `Knobs.Hold`. The others it applies itself.
const SYSTEM_KNOBS: [&str; 1] = ["power.profile"];
/// A synthetic state field: a session rule holds the profile, so the profile shown is not the user's pick.
const PROFILE_BY_RULE: &str = "power.profile_by_rule";
const NOTIFICATIONS: &str = "org.freedesktop.Notifications";
const NOTIFICATIONS_PATH: &str = "/org/freedesktop/Notifications";
const COMPOSITOR: &str = "org.andromeda.Compositor1";
const APPS_PATH: &str = "/org/andromeda/Compositor1/Apps";
const APPS: &str = "org.andromeda.Compositor1.Apps";
/// `List()` keeps the compositor sampling for 2 min; asking every minute keeps it on while a per-app rule is on.
const APPS_EVERY: std::time::Duration = std::time::Duration::from_secs(60);

/// Alert strings until the i18n catalog reaches this process: key -> (summary, body).
fn alert_text(key: &str) -> (&'static str, &'static str) {
    match key {
        "low_power_plan" => {
            ("Low Power Mode is on", "The battery is low. The desktop uses less power until you plug in.")
        }
        "battery_critical" => ("Battery critically low", "Plug in now. The computer will soon shut down."),
        "drainer" => ("{app} uses a lot of energy", "It used {cpu} % of a CPU core for 2 minutes."),
        _ => ("The desktop changed a setting", "Open Settings > Battery and Power to see why."),
    }
}

/// Opens Activity on one app, as its alert's Open action asks.
fn open_activity(unit: &str) {
    match tokio::process::Command::new("noctalia").args(["msg", "activity-open", unit]).spawn() {
        Ok(mut child) => {
            tokio::spawn(async move { child.wait().await });
        }
        Err(e) => log::warn!("session: could not open Activity: {e}"),
    }
}

/// A readable name from an app unit: `app-gnome-org.mozilla.firefox-1234.scope` -> `Firefox`.
fn app_name(unit: &str) -> String {
    let base = unit.strip_prefix("app-").unwrap_or(unit);
    let base = base.rsplit_once('.').filter(|(_, ext)| *ext == "scope" || *ext == "service").map_or(base, |(b, _)| b);
    let base = base.split('@').next().unwrap_or(base);
    let base =
        base.rsplit_once('-').filter(|(_, tail)| tail.chars().all(|c| c.is_ascii_hexdigit())).map_or(base, |(b, _)| b);
    // A launcher prefix (`gnome-`, `andromeda-`, `flatpak-`) comes before a reverse-DNS app id.
    let base = base.split_once('-').filter(|(_, id)| id.contains('.')).map_or(base, |(_, id)| id);
    let name = base.rsplit('.').next().unwrap_or(base);
    let mut chars = name.chars();
    chars.next().map_or_else(String::new, |first| first.to_uppercase().chain(chars).collect())
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
    /// The last Compositor1.Apps sample, for per-app rules.
    apps: Vec<AppRow>,
    /// Open per-app episodes: episode -> the compositor hold ids.
    app_held: HashMap<u64, Vec<String>>,
    /// Wakes the step loop when a new app sample lands.
    wake: Arc<Notify>,
}

type Shared = Arc<Mutex<Session>>;
/// The body of `InterfacesAdded`: interface -> properties.
type Interfaces = HashMap<String, HashMap<String, OwnedVariant>>;

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
            if !SYSTEM_KNOBS.contains(&hold.knob.as_str()) {
                continue;
            }
            let Some(value) = variant(&hold.value) else { continue };
            let reply = system
                .call_method(
                    Some(NAME),
                    knobs::PATH,
                    Some("org.andromeda.System1.Knobs"),
                    "Hold",
                    &(hold.knob.as_str(), value, rule),
                )
                .await;
            match reply.and_then(|r| r.body().deserialize::<String>()) {
                Ok(id) => ids.push(id),
                Err(e) => log::warn!("session: {rule} could not hold {}: {e}", hold.knob),
            }
        }
        ids
    }

    /// Places a per-app episode's holds on its app through Compositor1.Apps; a hold with a `when` only if it holds.
    async fn place_app(&self, session: &Connection, entry: &Entry) -> Vec<String> {
        let mut ids = Vec::new();
        let Some((rule, _)) = self.engine.rules().find(|(r, _)| r.id == entry.rule) else { return ids };
        let Some(app) = self.apps.iter().find(|a| a.0 == entry.instance) else { return ids };
        // Decided before the first call: the values borrow state that must not live across an await.
        let holds: Vec<&rules::Hold> = {
            let values = AppValues { state: &self.state, app };
            let values = RuleValues { state: &values, params: &rule.params };
            rule.holds
                .iter()
                .zip(&rule.hold_when)
                .filter(|(_, when)| when.as_ref().is_none_or(|w| w.holds(&values)))
                .map(|(hold, _)| hold)
                .collect()
        };
        for hold in holds {
            let reply = match (hold.knob.as_str(), hold.value.as_str()) {
                ("app.tier", Some(tier)) => {
                    session
                        .call_method(
                            Some(COMPOSITOR),
                            APPS_PATH,
                            Some(APPS),
                            "HoldTier",
                            &(entry.instance.as_str(), tier, entry.rule.as_str()),
                        )
                        .await
                }
                ("app.freeze", _) => {
                    session
                        .call_method(
                            Some(COMPOSITOR),
                            APPS_PATH,
                            Some(APPS),
                            "HoldFreeze",
                            &(entry.instance.as_str(), entry.rule.as_str()),
                        )
                        .await
                }
                _ => continue,
            };
            match reply.and_then(|r| r.body().deserialize::<String>()) {
                Ok(id) => ids.push(id),
                Err(e) => log::warn!("session: {} could not hold {} on {}: {e}", entry.rule, hold.knob, entry.instance),
            }
        }
        ids
    }

    async fn alert(&mut self, session: &Connection, entry: &Entry) {
        let Some(alert) = self.specs.get(&entry.rule).and_then(|s| s.alert.clone()) else { return };
        let (summary, body) = alert_text(&alert.text);
        let cpu = entry.trigger.iter().find(|(k, _)| k == "app.cpu_pct").and_then(|(_, v)| v.parse::<f64>().ok());
        let fill = |text: &str| {
            text.replace("{app}", &app_name(&entry.instance))
                .replace("{cpu}", &cpu.map_or_else(String::new, |c| format!("{c:.0}")))
        };
        let (summary, body) = (fill(summary), fill(body));
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
            .call_method(
                Some(NOTIFICATIONS),
                NOTIFICATIONS_PATH,
                Some(NOTIFICATIONS),
                "Notify",
                &(
                    "Andromeda",
                    0u32,
                    "battery-caution-symbolic",
                    summary.as_str(),
                    body.as_str(),
                    actions,
                    hints,
                    -1i32,
                ),
            )
            .await;
        match reply.and_then(|r| r.body().deserialize::<u32>()) {
            Ok(id) => {
                self.alerts.insert(id, entry.episode);
            }
            Err(e) => log::warn!("session: alert for {}: {e}", entry.rule),
        }
    }

    async fn commit(&mut self, system: &Connection, session: &Connection, changes: Changes) {
        let Changes { entries, apply, .. } = changes;
        for entry in entries {
            if let Err(e) = self.log.append(&entry) {
                log::warn!("explain log: {e}");
            }
            if let Ok(emitter) = SignalEmitter::new(session, explain::PATH) {
                let _ = explain::Explain::entry(&emitter, entry.to_dict()).await;
            }
            if entry.event == "started" {
                if entry.instance.is_empty() {
                    let ids = self.place(system, &entry.rule).await;
                    self.held.insert(entry.episode, (entry.rule.clone(), ids));
                } else {
                    let ids = self.place_app(session, &entry).await;
                    self.app_held.insert(entry.episode, ids);
                }
                self.alert(session, &entry).await;
                continue;
            }
            for id in self.app_held.remove(&entry.episode).unwrap_or_default() {
                // The hold is gone already when the app or the compositor ended.
                let _ = session.call_method(Some(COMPOSITOR), APPS_PATH, Some(APPS), "Release", &(id,)).await;
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
                    .call_method(
                        Some(NOTIFICATIONS),
                        NOTIFICATIONS_PATH,
                        Some(NOTIFICATIONS),
                        "CloseNotification",
                        &(note,),
                    )
                    .await;
            }
        }
        for (knob, value) in apply {
            if SYSTEM_KNOBS.contains(&knob) {
                continue;
            }
            if knob == "network.wifi_power_save" {
                let on = value == Some(KnobValue::Bool(true));
                if let Err(e) = nm::set_wifi_power_save(system, on).await {
                    log::warn!("session: Wi-Fi power save: {e:#}");
                }
            }
            publish(session, knob, value.as_ref()).await;
        }
    }

    /// Steps the rules on the current state, then the per-app rules on the last app sample.
    async fn step_all(&mut self, system: &Connection, session: &Connection) {
        let by_rule = self
            .held
            .values()
            .any(|(rule, _)| self.specs.get(rule).is_some_and(|s| s.holds.iter().any(|h| h.knob == "power.profile")));
        self.state.set_flag(PROFILE_BY_RULE, by_rule);
        let state = self.state.clone();
        let changes = self.engine.step(&state, Instant::now(), now_ms());
        self.commit(system, session, changes).await;
        let changes = {
            let apps = self.apps.clone();
            let values: Vec<AppValues<'_>> = apps.iter().map(|app| AppValues { state: &state, app }).collect();
            let rows: Vec<(String, &dyn crate::expr::Values)> =
                apps.iter().zip(&values).map(|(a, v)| (a.0.clone(), v as &dyn crate::expr::Values)).collect();
            self.engine.step_instances(&rows, Instant::now(), now_ms())
        };
        self.commit(system, session, changes).await;
    }

    /// The system part is back: places the holds of the open episodes again.
    async fn resume(&mut self, system: &Connection) {
        let open: Vec<(u64, String)> = self.held.iter().map(|(e, (rule, _))| (*e, rule.clone())).collect();
        for (episode, rule) in open {
            let ids = self.place(system, &rule).await;
            self.held.insert(episode, (rule, ids));
        }
    }

    /// A user change ended one of our system holds: that is the Undo of its episode.
    async fn released(&mut self, system: &Connection, session: &Connection, id: &str) {
        let episode = self.held.iter().find(|(_, (_, ids))| ids.iter().any(|i| i == id)).map(|(e, _)| *e);
        if let Some(episode) = episode {
            self.undo(system, session, episode).await;
        }
    }

    /// A notification action: Undo ends the episode; the default action of a per-app alert opens Activity on it.
    async fn action(&mut self, system: &Connection, session: &Connection, note: u32, action: &str) {
        match (self.alerts.get(&note).copied(), action) {
            (Some(episode), "undo") => {
                self.undo(system, session, episode).await;
            }
            (Some(episode), "default") => {
                if let Some((_, app)) = self.engine.instance_of(episode) {
                    open_activity(app);
                }
            }
            _ => {}
        }
    }

    async fn undo(&mut self, system: &Connection, session: &Connection, episode: u64) -> bool {
        let Some(changes) = self.engine.undo(episode, now_ms()) else { return false };
        self.commit(system, session, changes).await;
        true
    }
}

/// `/org/andromeda/System1/Knobs` on the session bus: the effective value of each knob the session part owns, for
/// the clients that apply one (the shell for `display.brightness_step`).
#[derive(Default)]
struct SessionKnobs {
    effective: HashMap<String, OwnedVariant>,
}

#[interface(name = "org.andromeda.System1.Knobs")]
impl SessionKnobs {
    /// Knob -> value while a hold sets it; a knob with no hold is absent.
    #[zbus(property)]
    fn effective(&self) -> HashMap<String, OwnedVariant> {
        self.effective.iter().filter_map(|(k, v)| Some((k.clone(), v.try_clone().ok()?))).collect()
    }
}

async fn publish(session: &Connection, knob: &str, value: Option<&KnobValue>) {
    let Ok(iface) = session.object_server().interface::<_, SessionKnobs>(knobs::PATH).await else { return };
    {
        let mut knobs = iface.get_mut().await;
        match value.and_then(|v| OwnedVariant::try_from(Variant::from(v)).ok()) {
            Some(v) => knobs.effective.insert(knob.to_owned(), v),
            None => knobs.effective.remove(knob),
        };
    }
    let _ = iface.get().await.effective_changed(iface.signal_emitter()).await;
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
        let mut all: Vec<(u64, HashMap<String, OwnedVariant>)> =
            own.iter().map(|e| (e.time_ms * 1000, e.to_dict())).collect();
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
    let knobs = vec![
        Knob::new("power.profile", Merge::LowestPower),
        Knob::new("network.wifi_power_save", Merge::AnyTrue),
        Knob::new("display.brightness_step", Merge::Min),
    ];
    let (mut engine, refused) = Engine::new(knobs, rules, now_ms());
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
        let (rule, _) =
            inner.engine.rules().find(|(r, _)| r.id == id).ok_or_else(|| zbus::fdo::Error::Failed(id.into()))?;
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
        apps: Vec::new(),
        app_held: HashMap::new(),
        wake: Arc::new(Notify::new()),
    }));
    let session = zbus::connection::Builder::session()
        .serve_at(explain::PATH, SessionExplain { shared: shared.clone(), system: system.clone() })
        .serve_at(knobs::PATH, SessionKnobs::default())
        .serve_at(RULES, RulesObject { shared: shared.clone(), system: system.clone(), broken })
        .name(NAME)
        .build()
        .await?;
    tokio::spawn(follow_apps(session.clone(), shared.clone()));
    let _ = sd_notify::notify(&[sd_notify::NotifyState::Ready]);
    log::info!("serving {NAME} on the session bus");
    loop {
        if let Err(e) = follow(&system, &session, &shared).await {
            log::warn!("session: {e:#}");
        }
        bus::appeared(&system, NAME).await?;
    }
}

/// The next property change of an optional remote; false when its stream ended. Never resolves for `None`.
async fn changed(remote: &mut Option<Remote>) -> bool {
    match remote.as_mut() {
        Some(r) => matches!(r.next().await, Some(Ok(()))),
        None => std::future::pending().await,
    }
}

/// A stream of one signal, from `sender` when given.
async fn signals(conn: &Connection, sender: Option<&str>, interface: &str, member: &str) -> Result<MessageStream> {
    let mut rule = MatchRule::builder().msg_type(zbus::message::Type::Signal).interface(interface).member(member);
    if let Some(sender) = sender {
        rule = rule.sender(sender);
    }
    Ok(MessageStream::for_match_rule(rule.build()?, conn, None).await?)
}

/// One life of the system part: follows its state, steps the rules, and re-places the holds of open episodes.
async fn follow(system: &Connection, session: &Connection, shared: &Shared) -> Result<()> {
    let gone = bus::vanished(system, NAME);
    let mut power = Remote::new(system, NAME, power::PATH.to_owned(), "org.andromeda.System1.Power").await?;
    // The system part exports the battery once UPower reports it, which can be after this starts.
    let mut added = signals(system, Some(NAME), "org.freedesktop.DBus.ObjectManager", "InterfacesAdded").await?;
    let mut battery = Remote::new(system, NAME, BATTERY.to_owned(), "org.andromeda.System1.Battery").await.ok();
    let mut memory = Remote::new(system, NAME, memory::PATH.to_owned(), "org.andromeda.System1.Memory").await.ok();
    let mut thermal = Remote::new(system, NAME, thermal::PATH.to_owned(), "org.andromeda.System1.Thermal").await.ok();
    let mut released = signals(system, Some(NAME), "org.andromeda.System1.Knobs", "Released").await?;
    let mut actions = signals(session, None, NOTIFICATIONS, "ActionInvoked").await?;
    let me = system.unique_name().map(ToString::to_string).unwrap_or_default();
    {
        let mut inner = shared.lock().await;
        inner.state.set_power(&power.props);
        inner.state.set_battery(battery.as_ref().map(|b| &b.props));
        inner.state.set_memory(memory.as_ref().map(|m| &m.props));
        inner.state.set_thermal(thermal.as_ref().map(|t| &t.props));
        inner.resume(system).await;
    }
    tokio::pin!(gone);
    loop {
        shared.lock().await.step_all(system, session).await;
        let (deadline, wake) = {
            let inner = shared.lock().await;
            (inner.engine.deadline(), inner.wake.clone())
        };
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
            message = added.next() => {
                let message = message.context("InterfacesAdded stream ended")??;
                let Ok((path, _)) = message.body().deserialize::<(zbus::OwnedObjectPath, Interfaces)>() else { continue };
                if battery.is_none() && path.as_str() == BATTERY {
                    battery = Remote::new(system, NAME, BATTERY.to_owned(), "org.andromeda.System1.Battery").await.ok();
                    shared.lock().await.state.set_battery(battery.as_ref().map(|b| &b.props));
                }
            }
            message = released.next() => {
                let message = message.context("Released stream ended")??;
                let Ok((id, owner)) = message.body().deserialize::<(String, String)>() else { continue };
                if owner == me {
                    shared.lock().await.released(system, session, &id).await;
                }
            }
            alive = changed(&mut memory) => {
                if !alive {
                    memory = None;
                }
                shared.lock().await.state.set_memory(memory.as_ref().map(|m| &m.props));
            }
            alive = changed(&mut thermal) => {
                if !alive {
                    thermal = None;
                }
                shared.lock().await.state.set_thermal(thermal.as_ref().map(|t| &t.props));
            }
            message = actions.next() => {
                let message = message.context("ActionInvoked stream ended")??;
                let Ok((note, action)) = message.body().deserialize::<(u32, String)>() else { continue };
                shared.lock().await.action(system, session, note, &action).await;
            }
            () = sleep => {}
            () = wake.notified() => {}
        }
    }
}

/// Feeds per-app rules: `Compositor1.Apps` samples (`AppStats` every 30 s), asking `List()` each minute so the
/// compositor keeps sampling. Idle, with no `List()` call, while no per-app rule is on.
async fn follow_apps(session: Connection, shared: Shared) {
    let rule = MatchRule::builder()
        .msg_type(zbus::message::Type::Signal)
        .sender(COMPOSITOR)
        .path(APPS_PATH)
        .interface(APPS)
        .member("AppStats")
        .build();
    let stats = match rule {
        Ok(rule) => MessageStream::for_match_rule(rule, &session, None).await,
        Err(e) => Err(e),
    };
    let mut stats = match stats {
        Ok(stream) => stream,
        Err(e) => {
            log::warn!("session: no AppStats stream: {e}");
            return;
        }
    };
    let mut tick = tokio::time::interval(APPS_EVERY);
    loop {
        let rows: Option<Vec<AppRow>> = tokio::select! {
            _ = tick.tick() => {
                let wanted = shared.lock().await.engine.rules().any(|(r, on)| on && r.per.is_some());
                if !wanted {
                    continue;
                }
                match session.call_method(Some(COMPOSITOR), APPS_PATH, Some(APPS), "List", &()).await {
                    Ok(reply) => reply.body().deserialize().ok(),
                    // No compositor with the API (a nested or older one): per-app rules see no apps.
                    Err(_) => Some(Vec::new()),
                }
            }
            message = stats.next() => match message {
                Some(Ok(message)) => message.body().deserialize::<(Vec<AppRow>,)>().ok().map(|(rows,)| rows),
                _ => return,
            },
        };
        if let Some(rows) = rows {
            let mut inner = shared.lock().await;
            inner.apps = rows;
            inner.wake.notify_one();
        }
    }
}
