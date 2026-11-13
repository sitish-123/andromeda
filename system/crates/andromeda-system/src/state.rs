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
    // A drive whose SMART data fails, as "<model> (<what fails>)"; empty when none does.
    ("storage.drive_failing", Type::Bool),
    ("storage.drive", Type::Str),
    // Errors the `/` or `/home` file system counted, and that mount point.
    ("storage.fs_errors", Type::Num),
    ("storage.fs_mount", Type::Str),
    // From org.andromeda.System1.Network (NetworkManager): unknown, none, portal, limited or full.
    ("network.connectivity", Type::Str),
    ("network.metered", Type::Bool),
    ("network.vpn", Type::Bool),
    // From org.andromeda.Compositor1.Apps `Presenting`: an app captures the screen or a display mirrors.
    ("display.presenting", Type::Bool),
    // One app, for per-app rules (`per = "app"`): from org.andromeda.Compositor1.Apps.
    ("app.cpu_pct", Type::Num),
    ("app.rss_mb", Type::Num),
    ("app.visible", Type::Bool),
    ("app.focused", Type::Bool),
    // From Compositor1.Apps `Idle`: the user left the input alone, or every screen is off.
    ("session.idle", Type::Bool),
    // HealthPercent of the display battery: full capacity now / design capacity.
    ("battery.health_pct", Type::Num),
    // This boot and login, and the median of the earlier ones (absent below 5 of them); seconds.
    ("startup.boot_s", Type::Num),
    ("startup.boot_median_s", Type::Num),
    ("startup.login_s", Type::Num),
    ("startup.login_median_s", Type::Num),
    // One process, for `per = "process"` rules (procs.rs). Wakeups = voluntary context switches per second, all threads.
    ("proc.wakeups_s", Type::Num),
    ("proc.anon_mb", Type::Num),
    // Least-squares growth of anonymous memory over the last 2 h, and its fit; absent before 1 h of samples.
    ("proc.growth_mb_h", Type::Num),
    ("proc.growth_r2", Type::Num),
    // One crashed executable, for `per = "crash"` rules: crashes in the last 24 h.
    ("crash.count", Type::Num),
    // Every one of those crashes came with a compositor crash (crashes.rs AFTERMATH).
    ("crash.with_desktop", Type::Bool),
    // One systemd unit, for `per = "unit"` rules: the unit is in the failed state.
    ("unit.failed", Type::Bool),
    // It failed right after a compositor crash, so the desktop crash alert covers it.
    ("unit.with_desktop", Type::Bool),
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
        let health = props.and_then(|p| bus::get::<f64>(p, "HealthPercent")).filter(|h| *h > 0.0);
        self.put("battery.health_pct", health.map(Value::Num));
    }

    /// A number the session part computes itself; `None` clears it.
    pub fn set_num(&mut self, name: &'static str, value: Option<f64>) {
        self.put(name, value.map(Value::Num));
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
        let failing = props.and_then(|p| bus::get::<Vec<(String, String)>>(p, "FailingDrives"));
        self.put("storage.drive_failing", failing.as_ref().map(|f| Value::Bool(!f.is_empty())));
        let first = failing.and_then(|f| f.into_iter().next()).map(|(model, what)| format!("{model} ({what})"));
        self.put("storage.drive", Some(Value::Str(first.unwrap_or_default())));
        let errors = props.and_then(|p| bus::get::<Vec<(String, u64)>>(p, "FsErrors"));
        #[allow(clippy::cast_precision_loss)] // an error count
        self.put("storage.fs_errors", errors.as_ref().map(|e| Value::Num(e.iter().map(|(_, n)| *n as f64).sum())));
        let mount = errors.and_then(|e| e.into_iter().next()).map(|(m, _)| m).unwrap_or_default();
        self.put("storage.fs_mount", Some(Value::Str(mount)));
    }

    /// From `org.andromeda.System1.Network`.
    pub fn set_network(&mut self, props: Option<&Props>) {
        self.put("network.connectivity", props.and_then(|p| bus::get::<String>(p, "Connectivity")).map(Value::Str));
        self.put("network.metered", props.and_then(|p| bus::get::<bool>(p, "Metered")).map(Value::Bool));
        self.put("network.vpn", props.and_then(|p| bus::get::<bool>(p, "Vpn")).map(Value::Bool));
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

/// One instance of a `per` kind other than `app`: its key and its own fields.
pub type InstanceRow = (String, Vec<(&'static str, Value)>);

/// The session state plus one instance's own fields (`proc.*`, `crash.*`, `unit.*`).
pub struct InstanceValues<'a> {
    pub state: &'a Snapshot,
    pub fields: &'a [(&'static str, Value)],
}

impl Values for InstanceValues<'_> {
    fn value_of(&self, name: &str) -> Option<Value> {
        match self.fields.iter().find(|(n, _)| *n == name) {
            Some((_, v)) => Some(v.clone()),
            None if name.starts_with("proc.") || name.starts_with("crash.") || name.starts_with("unit.") => None,
            None => self.state.value_of(name),
        }
    }
}

#[cfg(test)]
#[allow(clippy::unwrap_used)]
mod tests {
    use super::Fields;

    // A shipped rule that names an unknown field or does not parse is dropped at runtime with only a log line.
    #[test]
    fn every_shipped_rule_file_loads_against_the_state_fields() {
        let dir = concat!(env!("CARGO_MANIFEST_DIR"), "/../../data/rules");
        let mut files = 0;
        for entry in std::fs::read_dir(dir).unwrap() {
            let path = entry.unwrap().path();
            if path.extension().is_some_and(|e| e == "toml") {
                let text = std::fs::read_to_string(&path).unwrap();
                let loaded = crate::rules::load(&text, &Fields).unwrap_or_else(|e| panic!("{}: {e}", path.display()));
                for rule in loaded {
                    if let Err((id, why)) = rule {
                        panic!("{}: rule {id}: {why}", path.display());
                    }
                }
                files += 1;
            }
        }
        assert!(files > 0, "no rule files in {dir}");
    }

    struct At(std::collections::HashMap<&'static str, crate::expr::Value>);

    impl crate::expr::Values for At {
        fn value_of(&self, name: &str) -> Option<crate::expr::Value> {
            self.0.get(name).cloned()
        }
    }

    /// The lowest `display.refresh_cap_hz` the shipped power rules hold once this state has lasted 10 min.
    fn shipped_refresh_cap(on_battery: bool, profile: &str) -> Option<i64> {
        use crate::expr::Value;
        let text =
            std::fs::read_to_string(concat!(env!("CARGO_MANIFEST_DIR"), "/../../data/rules/power.toml")).unwrap();
        let state = At(std::collections::HashMap::from([
            ("power.on_battery", Value::Bool(on_battery)),
            ("power.profile", Value::Str(profile.to_owned())),
            ("power.profile_by_rule", Value::Bool(false)),
            ("battery.percent", Value::Num(80.0)),
            ("battery.warning", Value::Str("none".to_owned())),
        ]));
        let t0 = std::time::Instant::now();
        let mut cap: Option<i64> = None;
        for rule in crate::rules::load(&text, &Fields).unwrap().into_iter().map(Result::unwrap) {
            let mut ep = crate::rules::Episode::default();
            ep.step(&rule, &state, t0);
            ep.step(&rule, &state, t0 + std::time::Duration::from_secs(600));
            if ep.active() {
                for hold in rule.holds.iter().filter(|h| h.knob == "display.refresh_cap_hz") {
                    let hz = hold.value.as_integer().unwrap();
                    cap = Some(cap.map_or(hz, |c| c.min(hz)));
                }
            }
        }
        cap
    }

    #[test]
    fn power_saver_caps_the_refresh_rate_only_on_battery() {
        assert_eq!(shipped_refresh_cap(false, "power-saver"), None);
        assert_eq!(shipped_refresh_cap(true, "power-saver"), Some(60));
        assert_eq!(shipped_refresh_cap(true, "balanced"), None);
    }
}
