//! The state fields rules may read (docs/system-health.md §4.1), filled from the System1 objects. A field the
//! source does not report is absent, so a rule that reads it is unknown, not false.

use std::collections::HashMap;

use crate::bus::{self, Props};
use crate::expr::{Schema, Type, Value, Values};

const PROFILES: &[&str] = &["power-saver", "balanced", "performance"];
/// UPower's warning levels in order; `unknown` is left out, so it reads as an absent field.
const WARNINGS: &[&str] = &["none", "discharging", "low", "critical", "action"];

const FIELDS: &[(&str, Type)] = &[
    ("power.on_battery", Type::Bool),
    ("power.profile", Type::Level(PROFILES)),
    ("battery.percent", Type::Num),
    ("battery.warning", Type::Level(WARNINGS)),
    ("battery.state", Type::Str),
    ("battery.time_to_empty_s", Type::Num),
    ("power.profile_by_rule", Type::Bool),
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
