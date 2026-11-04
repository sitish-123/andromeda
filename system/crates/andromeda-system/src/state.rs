//! The state fields rules may read (docs/system-health.md §4.1), filled from the System1 objects. A field the
//! source does not report is absent, so a rule that reads it is unknown, not false.

use std::collections::HashMap;

use crate::bus::{self, Props};
use crate::expr::{Schema, Type, Value, Values};

const PROFILES: &[&str] = &["power-saver", "balanced", "performance"];
/// UPower's warning levels in order; `unknown` is left out, so it reads as an absent field.
const WARNINGS: &[&str] = &["none", "discharging", "low", "critical", "action"];
/// `org.andromeda.System1.Memory.Level`, from PSI.
const MEMORY_LEVELS: &[&str] = &["normal", "medium", "critical"];

/// Not a rule field: the source of `battery.unplug_in_min`.
const LEARNED_UNPLUG: &str = "power.learned_unplug_min";

const FIELDS: &[(&str, Type)] = &[
    ("power.on_battery", Type::Bool),
    ("power.profile", Type::Level(PROFILES)),
    ("battery.percent", Type::Num),
    ("battery.warning", Type::Level(WARNINGS)),
    ("battery.state", Type::Str),
    ("battery.time_to_empty_s", Type::Num),
    ("battery.charge_limit_supported", Type::Bool),
    // Minutes to the learned unplug time; absent before 14 days of plug history.
    ("battery.unplug_in_min", Type::Num),
    ("power.profile_by_rule", Type::Bool),
    ("memory.level", Type::Level(MEMORY_LEVELS)),
    ("thermal.throttled", Type::Bool),
    // The filesystem (`/` or a separate `/home`) with the least free space, by percent.
    ("storage.free_pct", Type::Num),
    ("storage.free_gb", Type::Num),
    ("storage.mount", Type::Str),
    // One app, for per-app rules (`per = "app"`): from org.andromeda.Compositor1.Apps.
    ("app.cpu_pct", Type::Num),
    ("app.rss_mb", Type::Num),
    ("app.visible", Type::Bool),
    ("app.focused", Type::Bool),
];

#[derive(Clone, Debug, Default)]
pub struct Snapshot(HashMap<&'static str, Value>);

pub struct Fields;

impl Schema for Fields {
    fn type_of(&self, name: &str) -> Option<Type> {
        FIELDS.iter().find(|(n, _)| *n == name).map(|(_, t)| *t)
    }
}

impl Schema for Snapshot {
    fn type_of(&self, name: &str) -> Option<Type> {
        Fields.type_of(name)
    }
}

impl Values for Snapshot {
    fn value_of(&self, name: &str) -> Option<Value> {
        self.0.get(name).cloned()
    }
}

impl Snapshot {
    /// From `org.andromeda.System1.Power`.
    pub fn set_power(&mut self, props: &Props) {
        self.put("power.on_battery", bus::get::<bool>(props, "OnBattery").map(Value::Bool));
        let profile = bus::get::<String>(props, "Profile").filter(|p| PROFILES.contains(&p.as_str()));
        self.put("power.profile", profile.map(Value::Str));
        let supported = bus::get::<bool>(props, "ChargeLimitSupported");
        self.put("battery.charge_limit_supported", supported.map(Value::Bool));
        let learned = bus::get::<i32>(props, "LearnedUnplugMin").filter(|m| *m >= 0);
        self.put(LEARNED_UNPLUG, learned.map(|m| Value::Num(m.into())));
    }

    /// Sets the fields that follow the clock; `now_s` is unix time. Call before each step.
    pub fn set_clock(&mut self, now_s: u64) {
        #[allow(clippy::cast_precision_loss)] // minutes of a day
        let now_min = (now_s % 86_400 / 60) as f64;
        let left = match self.0.get(LEARNED_UNPLUG) {
            Some(Value::Num(at)) => Some(Value::Num((at - now_min).rem_euclid(1440.0))),
            _ => None,
        };
        self.put("battery.unplug_in_min", left);
    }

    /// Whether a field moves with the clock now, so steps must come without a state change.
    pub fn clock_runs(&self) -> bool {
        self.0.contains_key("battery.unplug_in_min") && self.0.get("power.on_battery") == Some(&Value::Bool(false))
    }

    /// From the display device's `org.andromeda.System1.Battery`; a missing or absent battery clears the fields.
    pub fn set_battery(&mut self, props: Option<&Props>) {
        let props = props.filter(|p| bus::get::<bool>(p, "Present").unwrap_or(false));
        let get_str = |key| props.and_then(|p| bus::get::<String>(p, key));
        self.put("battery.percent", props.and_then(|p| bus::get::<f64>(p, "Percent")).map(Value::Num));
        let warning = get_str("Warning").filter(|w| WARNINGS.contains(&w.as_str()));
        self.put("battery.warning", warning.map(Value::Str));
        self.put("battery.state", get_str("State").filter(|s| s != "unknown").map(Value::Str));
        #[allow(clippy::cast_precision_loss)] // seconds
        let tte = props.and_then(|p| bus::get::<i64>(p, "TimeToEmptyS")).filter(|s| *s > 0).map(|s| s as f64);
        self.put("battery.time_to_empty_s", tte.map(Value::Num));
    }

    /// From `org.andromeda.System1.Memory`; `None` when the system part has no memory source.
    pub fn set_memory(&mut self, props: Option<&Props>) {
        let level = props.and_then(|p| bus::get::<String>(p, "Level")).filter(|l| MEMORY_LEVELS.contains(&l.as_str()));
        self.put("memory.level", level.map(Value::Str));
    }

    /// From `org.andromeda.System1.Thermal`: a zone is past a trip point.
    pub fn set_thermal(&mut self, props: Option<&Props>) {
        self.put("thermal.throttled", props.and_then(|p| bus::get::<bool>(p, "Throttled")).map(Value::Bool));
    }

    /// From `org.andromeda.System1.Storage.Mounts` (mount point, type, free bytes, size bytes).
    pub fn set_storage(&mut self, props: Option<&Props>) {
        let mounts = props.and_then(|p| bus::get::<Vec<(String, String, u64, u64)>>(p, "Mounts")).unwrap_or_default();
        #[allow(clippy::cast_precision_loss)] // bytes as a percent and GB
        let least = mounts
            .iter()
            .filter(|(_, _, _, size)| *size > 0)
            .map(|(point, _, free, size)| (point.clone(), *free as f64 * 100.0 / *size as f64, *free as f64 / 1e9))
            .min_by(|a, b| a.1.total_cmp(&b.1));
        self.put("storage.free_pct", least.as_ref().map(|l| Value::Num(l.1)));
        self.put("storage.free_gb", least.as_ref().map(|l| Value::Num(l.2)));
        self.put("storage.mount", least.map(|l| Value::Str(l.0)));
    }

    /// A field the session part computes itself.
    pub fn set_flag(&mut self, name: &'static str, on: bool) {
        self.put(name, Some(Value::Bool(on)));
    }

    fn put(&mut self, name: &'static str, value: Option<Value>) {
        match value {
            Some(v) => self.0.insert(name, v),
            None => self.0.remove(name),
        };
    }
}

/// One app's sample from `Compositor1.Apps`: (unit, visible, focused, tier, frozen, cpu % of one core, rss bytes).
pub type AppRow = (String, bool, bool, String, bool, f64, u64);

/// The session state plus one app's `app.*` fields.
pub struct AppValues<'a> {
    pub state: &'a Snapshot,
    pub app: &'a AppRow,
}

impl Values for AppValues<'_> {
    fn value_of(&self, name: &str) -> Option<Value> {
        let (_, visible, focused, _, _, cpu, rss) = self.app;
        #[allow(clippy::cast_precision_loss)] // MiB
        let rss_mb = *rss as f64 / 1_048_576.0;
        match name {
            "app.cpu_pct" => Some(Value::Num(*cpu)),
            "app.rss_mb" => Some(Value::Num(rss_mb)),
            "app.visible" => Some(Value::Bool(*visible)),
            "app.focused" => Some(Value::Bool(*focused)),
            _ => self.state.value_of(name),
        }
    }
}
