//! Rules as data (docs/system-health.md §4): TOML files, checked when they load, and the episode machine that turns
//! condition changes into start and end events. Pure: the caller passes the time and the current values.

use std::collections::BTreeMap;
use std::time::{Duration, Instant};

use serde::Deserialize;

use crate::expr::{self, Expr, Type, Value, Values};

#[derive(Clone, Copy, Debug, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "lowercase")]
pub enum Scope {
    System,
    Session,
}

/// Higher wins (§5): safety > user > automatic.
#[derive(Clone, Copy, Debug, Deserialize, PartialEq, Eq, PartialOrd, Ord)]
#[serde(rename_all = "lowercase")]
pub enum Priority {
    Automatic,
    User,
    Safety,
}

#[derive(Clone, Debug, Deserialize, PartialEq)]
#[serde(deny_unknown_fields)]
pub struct Hold {
    pub knob: String,
    pub value: toml::Value,
    /// Per-app rules only: the hold is placed when the episode starts only if this holds for the app.
    #[serde(default)]
    pub when: Option<String>,
}

#[derive(Clone, Debug, Deserialize, PartialEq)]
pub struct Alert {
    pub urgency: String,
    #[serde(default)]
    pub action: Option<String>,
    /// Key of the alert's strings in the i18n catalog.
    pub text: String,
    /// A state field: while the episode is open, the alert comes again each time it falls past one more whole unit.
    #[serde(default)]
    pub again: Option<String>,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct RuleFile {
    rule: Vec<RuleSpec>,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct RuleSpec {
    id: String,
    scope: Scope,
    priority: Priority,
    #[serde(default)]
    params: BTreeMap<String, toml::Value>,
    when: String,
    #[serde(default, rename = "for")]
    for_: Option<String>,
    clear: String,
    #[serde(default)]
    dwell: Option<String>,
    #[serde(default)]
    holds: Vec<Hold>,
    #[serde(default)]
    alert: Option<Alert>,
    #[serde(default)]
    per: Option<String>,
    #[serde(default)]
    largest_first: Option<String>,
}

/// A loaded rule. Users may change only `enabled` and the values of declared params.
#[derive(Debug)]
pub struct Rule {
    pub id: String,
    pub scope: Scope,
    pub priority: Priority,
    pub params: BTreeMap<String, Value>,
    pub when: Expr,
    pub hold_for: Duration,
    pub clear: Expr,
    pub dwell: Duration,
    pub holds: Vec<Hold>,
    /// Each hold's compiled `when`, in the order of `holds`.
    pub hold_when: Vec<Option<Expr>>,
    pub alert: Option<Alert>,
    /// `app`: one episode per app, read from the `app.*` fields; its holds go to that app, not to the knobs.
    pub per: Option<String>,
    /// A numeric `app.*` field: per step, only the largest app that is due starts; the others wait `for` again.
    pub largest_first: Option<String>,
}

/// What a per-app rule may hold on its app (the compositor's `HoldTier` and `HoldFreeze`).
pub const APP_HOLDS: [&str; 2] = ["app.tier", "app.freeze"];

/// `0s`, `30s`, `2m`, `1h`.
pub fn duration(text: &str) -> Result<Duration, String> {
    let (number, unit) = text.split_at(text.find(|c: char| !c.is_ascii_digit()).unwrap_or(text.len()));
    let n: u64 = number.parse().map_err(|_| format!("bad duration {text:?}"))?;
    let seconds = match unit {
        "s" => n,
        "m" => n * 60,
        "h" => n * 3600,
        _ => return Err(format!("bad duration {text:?}: use s, m or h")),
    };
    Ok(Duration::from_secs(seconds))
}

fn param_value(value: &toml::Value) -> Option<Value> {
    match value {
        toml::Value::Integer(i) => {
            #[allow(clippy::cast_precision_loss)] // params are small thresholds
            let n = *i as f64;
            Some(Value::Num(n))
        }
        toml::Value::Float(f) => Some(Value::Num(*f)),
        toml::Value::Boolean(b) => Some(Value::Bool(*b)),
        _ => None,
    }
}

/// The state schema plus one rule's params.
struct RuleSchema<'a> {
    state: &'a dyn expr::Schema,
    params: &'a BTreeMap<String, Value>,
}

impl expr::Schema for RuleSchema<'_> {
    fn type_of(&self, name: &str) -> Option<Type> {
        match self.params.get(name) {
            Some(Value::Num(_)) => Some(Type::Num),
            Some(Value::Bool(_)) => Some(Type::Bool),
            Some(Value::Str(_)) => Some(Type::Str),
            None => self.state.type_of(name),
        }
    }
}

/// Values of the state plus one rule's params.
pub struct RuleValues<'a> {
    pub state: &'a dyn Values,
    pub params: &'a BTreeMap<String, Value>,
}

impl Values for RuleValues<'_> {
    fn value_of(&self, name: &str) -> Option<Value> {
        self.params.get(name).cloned().or_else(|| self.state.value_of(name))
    }
}

/// A rule, or the id of a broken rule and why it is broken.
pub type Loaded = Result<Rule, (String, String)>;

/// Loads one rule file. A broken rule is returned as an error with its id, so the others still load and Settings
/// can show why that one is off.
pub fn load(text: &str, state: &dyn expr::Schema) -> Result<Vec<Loaded>, String> {
    let file: RuleFile = toml::from_str(text).map_err(|e| e.to_string())?;
    Ok(file.rule.into_iter().map(|spec| compile(spec, state)).collect())
}

fn compile(spec: RuleSpec, state: &dyn expr::Schema) -> Loaded {
    let id = spec.id.clone();
    let fail = |what: &str, e: &dyn std::fmt::Display| (id.clone(), format!("{what}: {e}"));
    let mut params = BTreeMap::new();
    for (name, value) in &spec.params {
        let value = param_value(value).ok_or_else(|| fail(name, &"a param is a number or a bool"))?;
        params.insert(name.clone(), value);
    }
    let schema = RuleSchema { state, params: &params };
    let when = Expr::parse(&spec.when, &schema).map_err(|e| fail("when", &e))?;
    let clear = Expr::parse(&spec.clear, &schema).map_err(|e| fail("clear", &e))?;
    let hold_for = duration(spec.for_.as_deref().unwrap_or("0s")).map_err(|e| fail("for", &e))?;
    let dwell = duration(spec.dwell.as_deref().unwrap_or("0s")).map_err(|e| fail("dwell", &e))?;
    if let Some(per) = spec.per.as_deref() {
        if per != "app" {
            return Err(fail("per", &"only \"app\""));
        }
        if let Some(h) = spec.holds.iter().find(|h| !APP_HOLDS.contains(&h.knob.as_str())) {
            return Err(fail("holds", &format!("a per-app rule holds only {APP_HOLDS:?}, not {:?}", h.knob)));
        }
    }
    if let Some(field) = spec.largest_first.as_deref() {
        if spec.per.is_none() {
            return Err(fail("largest_first", &"only a per-app rule"));
        }
        if expr::Schema::type_of(&schema, field) != Some(Type::Num) {
            return Err(fail("largest_first", &format!("{field:?} is not a number field")));
        }
    }
    let mut hold_when = Vec::new();
    for hold in &spec.holds {
        hold_when.push(match (&hold.when, spec.per.is_some()) {
            (Some(when), true) => Some(Expr::parse(when, &schema).map_err(|e| fail("holds.when", &e))?),
            (Some(_), false) => return Err(fail("holds.when", &"only a per-app rule has a hold condition")),
            (None, _) => None,
        });
    }
    Ok(Rule {
        id: spec.id,
        scope: spec.scope,
        priority: spec.priority,
        params,
        when,
        hold_for,
        clear,
        dwell,
        holds: spec.holds,
        hold_when,
        alert: spec.alert,
        per: spec.per,
        largest_first: spec.largest_first,
    })
}

/// One rule's entry in the user's settings file.
#[derive(Debug, PartialEq)]
pub struct Setting {
    pub enabled: bool,
    pub params: Vec<(String, Value)>,
}

/// Parses the user's settings file: `["<rule id>"]` tables with `enabled` and param values (numbers or bools).
/// Keys of other types are skipped; the engine checks each param against the rule.
pub fn parse_settings(text: &str) -> Result<BTreeMap<String, Setting>, String> {
    let table: toml::Table = toml::from_str(text).map_err(|e| format!("settings: {e}"))?;
    Ok(table
        .iter()
        .filter_map(|(id, value)| {
            let entries = value.as_table()?;
            let enabled = entries.get("enabled").and_then(toml::Value::as_bool).unwrap_or(true);
            let params = entries
                .iter()
                .filter(|(k, _)| *k != "enabled")
                .filter_map(|(k, v)| Some((k.clone(), param_value(v)?)))
                .collect();
            Some((id.clone(), Setting { enabled, params }))
        })
        .collect())
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub enum End {
    Cleared,
    Undone,
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub enum Event {
    /// Place the holds and send the alert (once per episode).
    Started,
    /// Release the holds. After `Undone` the episode stays open, muted, until it clears.
    Ended(End),
}

#[derive(Clone, Copy, Debug, PartialEq)]
enum Phase {
    Idle,
    Pending { since: Instant },
    Active { since: Instant, muted: bool },
}

/// One rule's episode: `when` held for `for` starts it; `clear` after at least `dwell` ends it.
#[derive(Debug)]
pub struct Episode {
    phase: Phase,
}

impl Default for Episode {
    fn default() -> Self {
        Self { phase: Phase::Idle }
    }
}

impl Episode {
    pub fn active(&self) -> bool {
        matches!(self.phase, Phase::Active { muted: false, .. })
    }

    /// Re-evaluates at `now`; call on every state change and at `deadline()`.
    pub fn step(&mut self, rule: &Rule, values: &dyn Values, now: Instant) -> Option<Event> {
        let values = RuleValues { state: values, params: &rule.params };
        match self.phase {
            Phase::Idle if rule.when.holds(&values) => {
                if rule.hold_for.is_zero() {
                    self.phase = Phase::Active { since: now, muted: false };
                    Some(Event::Started)
                } else {
                    self.phase = Phase::Pending { since: now };
                    None
                }
            }
            Phase::Pending { .. } if !rule.when.holds(&values) => {
                self.phase = Phase::Idle;
                None
            }
            Phase::Pending { since } if now - since >= rule.hold_for => {
                self.phase = Phase::Active { since: now, muted: false };
                Some(Event::Started)
            }
            Phase::Active { since, muted } if now - since >= rule.dwell && rule.clear.holds(&values) => {
                self.phase = Phase::Idle;
                (!muted).then_some(Event::Ended(End::Cleared))
            }
            Phase::Idle | Phase::Pending { .. } | Phase::Active { .. } => None,
        }
    }

    /// Restarts the `for` wait of a pending episode.
    pub fn wait_again(&mut self, now: Instant) {
        if let Phase::Pending { .. } = self.phase {
            self.phase = Phase::Pending { since: now };
        }
    }

    /// The user's Undo: release now, and stay quiet until this episode clears.
    pub fn undo(&mut self) -> Option<Event> {
        match self.phase {
            Phase::Active { since, muted: false } => {
                self.phase = Phase::Active { since, muted: true };
                Some(Event::Ended(End::Undone))
            }
            _ => None,
        }
    }

    /// When `step` must run again even if no value changes: the end of `for`, or of `dwell` while it is after `now`
    /// (past the dwell only a state change can clear the episode).
    pub fn deadline(&self, rule: &Rule, now: Instant) -> Option<Instant> {
        match self.phase {
            Phase::Pending { since } => Some(since + rule.hold_for),
            Phase::Active { since, .. } if since + rule.dwell > now => Some(since + rule.dwell),
            _ => None,
        }
    }
}

#[cfg(test)]
#[allow(clippy::unwrap_used)]
mod tests {
    use std::collections::HashMap;
    use std::time::{Duration, Instant};

    use super::{End, Episode, Event, load, parse_settings};
    use crate::expr::{Schema, Type, Value, Values};

    struct State(HashMap<&'static str, Value>);

    impl Schema for State {
        fn type_of(&self, name: &str) -> Option<Type> {
            match name {
                "power.on_battery" => Some(Type::Bool),
                "battery.percent" => Some(Type::Num),
                _ => None,
            }
        }
    }

    impl Values for State {
        fn value_of(&self, name: &str) -> Option<Value> {
            self.0.get(name).cloned()
        }
    }

    fn at(on_battery: bool, percent: f64) -> State {
        State(HashMap::from([("power.on_battery", Value::Bool(on_battery)), ("battery.percent", Value::Num(percent))]))
    }

    const RULE: &str = r#"
[[rule]]
id = "power.low_power_plan"
scope = "session"
priority = "automatic"
params = { low_pct = 20, margin_pct = 5 }
when = "power.on_battery and battery.percent <= low_pct"
for = "10s"
clear = "not power.on_battery or battery.percent >= low_pct + margin_pct"
dwell = "2m"
holds = [{ knob = "power.profile", value = "power-saver" }]
"#;

    #[test]
    fn episode_waits_for_and_dwell_and_clear() {
        let rule = load(RULE, &at(false, 0.0)).unwrap().pop().unwrap().unwrap();
        let mut ep = Episode::default();
        let t0 = Instant::now();
        assert_eq!(ep.step(&rule, &at(true, 19.0), t0), None);
        assert_eq!(ep.deadline(&rule, t0), Some(t0 + Duration::from_secs(10)));
        assert_eq!(ep.step(&rule, &at(true, 19.0), t0 + Duration::from_secs(10)), Some(Event::Started));
        // Charger in after 30 s: clear holds, but dwell keeps the episode until 2 min.
        assert_eq!(ep.step(&rule, &at(false, 19.0), t0 + Duration::from_secs(40)), None);
        assert_eq!(ep.step(&rule, &at(false, 19.0), t0 + Duration::from_secs(130)), Some(Event::Ended(End::Cleared)));
    }

    #[test]
    fn an_episode_past_its_dwell_has_no_deadline_until_it_clears() {
        let rule = load(RULE, &at(false, 0.0)).unwrap().pop().unwrap().unwrap();
        let mut ep = Episode::default();
        let t0 = Instant::now();
        ep.step(&rule, &at(true, 19.0), t0);
        assert_eq!(ep.step(&rule, &at(true, 19.0), t0 + Duration::from_secs(10)), Some(Event::Started));
        let later = t0 + Duration::from_secs(300);
        assert_eq!(ep.step(&rule, &at(true, 19.0), later), None, "still low: no clear");
        assert_eq!(ep.deadline(&rule, later), None, "a past deadline makes the session loop spin");
    }

    #[test]
    fn a_short_dip_does_not_start() {
        let rule = load(RULE, &at(false, 0.0)).unwrap().pop().unwrap().unwrap();
        let mut ep = Episode::default();
        let t0 = Instant::now();
        assert_eq!(ep.step(&rule, &at(true, 19.0), t0), None);
        assert_eq!(ep.step(&rule, &at(true, 21.0), t0 + Duration::from_secs(5)), None);
        assert_eq!(ep.step(&rule, &at(true, 21.0), t0 + Duration::from_secs(20)), None);
        assert!(!ep.active());
    }

    #[test]
    fn undo_mutes_until_the_episode_clears() {
        let rule = load(RULE, &at(false, 0.0)).unwrap().pop().unwrap().unwrap();
        let mut ep = Episode::default();
        let t0 = Instant::now();
        ep.step(&rule, &at(true, 19.0), t0);
        ep.step(&rule, &at(true, 19.0), t0 + Duration::from_secs(10));
        assert_eq!(ep.undo(), Some(Event::Ended(End::Undone)));
        assert_eq!(ep.step(&rule, &at(true, 15.0), t0 + Duration::from_secs(60)), None);
        assert_eq!(ep.step(&rule, &at(false, 15.0), t0 + Duration::from_secs(200)), None);
        // The next discharge starts a new episode.
        assert_eq!(ep.step(&rule, &at(true, 15.0), t0 + Duration::from_secs(300)), None);
        assert_eq!(ep.step(&rule, &at(true, 15.0), t0 + Duration::from_secs(310)), Some(Event::Started));
    }

    #[test]
    fn settings_parse_enabled_and_params() {
        let settings = parse_settings("[\"power.low_power_plan\"]\nenabled = false\nlow_pct = 15\n").unwrap();
        let setting = &settings["power.low_power_plan"];
        assert!(!setting.enabled);
        assert_eq!(setting.params, vec![("low_pct".to_owned(), Value::Num(15.0))]);
    }

    #[test]
    fn a_broken_rule_names_itself_and_leaves_the_others() {
        let text = format!(
            "{RULE}\n[[rule]]\nid = \"bad\"\nscope = \"session\"\npriority = \"automatic\"\nwhen = \"battery.nope\"\nclear = \"true\"\n"
        );
        let rules = load(&text, &at(false, 0.0)).unwrap();
        assert!(rules[0].is_ok());
        let (id, why) = rules[1].as_ref().unwrap_err();
        assert_eq!(id, "bad");
        assert!(why.contains("unknown name"), "{why}");
    }
}
