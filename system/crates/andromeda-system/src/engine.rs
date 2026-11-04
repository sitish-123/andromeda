//! The rule engine (docs/system-health.md §4-§6): runs each rule's episode, places and releases its holds on the
//! knobs, and writes one explain entry per episode event. Pure: the caller passes the time and applies the knob
//! values it gets back.

use std::collections::BTreeMap;
use std::time::Instant;

use crate::explain::Entry;
use crate::expr::{Value, Values};
use crate::holds::{Hold, Knob, KnobValue, Source};
use crate::rules::{End, Episode, Event, Priority, Rule, RuleValues};

struct Slot {
    rule: Rule,
    holds: Vec<(&'static str, KnobValue)>,
    episode: Episode,
    id: u64,
    enabled: bool,
    /// A per-app rule's episodes by app, with each one's id.
    instances: BTreeMap<String, (Episode, u64)>,
}

/// What one call changed: entries for the explain log, and the knobs whose effective value moved (`None` = no
/// baseline and no hold: the owner drops its hold on the backend).
#[derive(Debug, Default, PartialEq)]
pub struct Changes {
    pub entries: Vec<Entry>,
    pub apply: Vec<(&'static str, Option<KnobValue>)>,
    /// Client holds that a user change ended: id and owner. The owner treats it as the Undo of its episode.
    pub released: Vec<(u64, String)>,
}

pub struct Engine {
    slots: Vec<Slot>,
    knobs: BTreeMap<&'static str, Knob>,
    next_id: u64,
}

fn text(value: Option<&KnobValue>) -> String {
    value.map_or_else(|| "none".to_owned(), ToString::to_string)
}

fn value_text(value: Option<Value>) -> String {
    match value {
        Some(Value::Num(n)) => n.to_string(),
        Some(Value::Bool(b)) => b.to_string(),
        Some(Value::Str(s)) => s,
        None => "unknown".to_owned(),
    }
}

impl Engine {
    /// `first_id` seeds episode ids; the caller passes the wall clock in ms so ids stay unique across restarts.
    /// Returns the rules it refused, with why: a hold on an unknown knob, a value of the wrong type, or the user
    /// priority (only `Knobs.Set` acts as the user).
    pub fn new(knobs: Vec<Knob>, rules: Vec<Rule>, first_id: u64) -> (Self, Vec<(String, String)>) {
        let knobs: BTreeMap<_, _> = knobs.into_iter().map(|k| (k.name, k)).collect();
        let mut refused = Vec::new();
        let mut slots = Vec::new();
        for rule in rules {
            if rule.per.is_some() {
                let slot = Slot {
                    rule,
                    holds: Vec::new(),
                    episode: Episode::default(),
                    id: 0,
                    enabled: true,
                    instances: BTreeMap::new(),
                };
                slots.push(slot);
                continue;
            }
            let holds: Result<Vec<_>, String> = rule
                .holds
                .iter()
                .map(|h| {
                    let knob = knobs.get(h.knob.as_str()).ok_or_else(|| format!("unknown knob {:?}", h.knob))?;
                    let value = KnobValue::from_toml(&h.value, knob.merge)
                        .ok_or_else(|| format!("{} cannot take {}", h.knob, h.value))?;
                    Ok((knob.name, value))
                })
                .collect();
            match holds {
                Ok(_) if rule.priority == Priority::User => {
                    refused.push((rule.id.clone(), "a rule is automatic or safety".to_owned()));
                }
                Ok(holds) => slots.push(Slot {
                    rule,
                    holds,
                    episode: Episode::default(),
                    id: 0,
                    enabled: true,
                    instances: BTreeMap::new(),
                }),
                Err(why) => refused.push((rule.id.clone(), why)),
            }
        }
        (Self { slots, knobs, next_id: first_id }, refused)
    }

    pub fn knob(&self, name: &str) -> Option<&Knob> {
        self.knobs.get(name)
    }

    pub fn knobs(&self) -> impl Iterator<Item = &Knob> {
        self.knobs.values()
    }

    /// Re-evaluates every rule; call on each state change and at `deadline()`.
    pub fn step(&mut self, values: &dyn Values, now: Instant, time_ms: u64) -> Changes {
        let mut changes = Changes::default();
        for i in 0..self.slots.len() {
            let slot = &mut self.slots[i];
            if !slot.enabled || slot.rule.per.is_some() {
                continue;
            }
            if let Some(event) = slot.episode.step(&slot.rule, values, now) {
                let read = match event {
                    Event::Started => &slot.rule.when,
                    Event::Ended(_) => &slot.rule.clear,
                };
                let state = RuleValues { state: values, params: &slot.rule.params };
                let trigger = read.names().into_iter().map(|n| (n.to_owned(), value_text(state.value_of(n)))).collect();
                self.apply(i, event, trigger, String::new(), time_ms, &mut changes);
            }
        }
        changes
    }

    /// The user's Undo of an episode, from the explain log or a notification action. `None` when it is not running.
    pub fn undo(&mut self, episode: u64, time_ms: u64) -> Option<Changes> {
        if let Some(changes) = self.undo_instance(episode, time_ms) {
            return Some(changes);
        }
        let i = self.slots.iter().position(|s| s.id == episode && s.episode.active())?;
        let event = self.slots[i].episode.undo()?;
        let mut changes = Changes::default();
        self.apply(i, event, Vec::new(), "undo".to_owned(), time_ms, &mut changes);
        Some(changes)
    }

    /// The user's choice (`Knobs.Set`). It undoes each automatic episode that holds this knob, then becomes the
    /// baseline.
    pub fn set_baseline(&mut self, knob: &str, value: KnobValue, time_ms: u64) -> Option<Changes> {
        let name = self.knobs.get(knob)?.name;
        let mut changes = Changes::default();
        for i in 0..self.slots.len() {
            let slot = &mut self.slots[i];
            let holds_knob = slot.holds.iter().any(|(k, _)| *k == name);
            if slot.rule.priority == Priority::Automatic
                && holds_knob
                && let Some(event) = slot.episode.undo()
            {
                let note = format!("you changed {name}");
                self.apply(i, event, Vec::new(), note, time_ms, &mut changes);
            }
        }
        let k = self.knobs.get_mut(name)?;
        let before = k.effective().0;
        let rules: Vec<&str> = self.slots.iter().map(|s| s.rule.id.as_str()).collect();
        k.holds.retain(|h| {
            let client = h.priority == Priority::Automatic && !rules.contains(&h.owner.as_str());
            if client {
                changes.released.push((h.id, h.owner.clone()));
            }
            !client
        });
        k.baseline = Some(value);
        let after = k.effective().0;
        if after != before {
            changes.apply.retain(|(n, _)| *n != name);
            changes.apply.push((name, after));
        }
        Some(changes)
    }

    /// An automatic hold from a client (the session part for its rules). `owner` is the client's bus name: its
    /// holds end when it leaves the bus. Returns the hold id and the knob value to apply, if it moved.
    pub fn hold(
        &mut self,
        knob: &str,
        owner: &str,
        reason: &str,
        value: &toml::Value,
        time_ms: u64,
    ) -> Result<(u64, Changes), String> {
        let k = self.knobs.get_mut(knob).ok_or_else(|| format!("unknown knob {knob:?}"))?;
        let value = KnobValue::from_toml(value, k.merge).ok_or_else(|| format!("{knob} cannot take {value}"))?;
        let id = self.next_id;
        self.next_id += 1;
        let before = k.effective().0;
        let (owner, reason) = (owner.to_owned(), reason.to_owned());
        k.holds.push(Hold { id, owner, priority: Priority::Automatic, value, reason, since_ms: time_ms });
        let after = k.effective().0;
        let mut changes = Changes::default();
        if after != before {
            changes.apply.push((k.name, after));
        }
        Ok((id, changes))
    }

    /// Ends client holds: one by id (only its owner may), or all of `owner`'s when `id` is `None`.
    pub fn release(&mut self, owner: &str, id: Option<u64>) -> Changes {
        let mut changes = Changes::default();
        for k in self.knobs.values_mut() {
            let before = k.effective().0;
            k.holds.retain(|h| h.owner != owner || id.is_some_and(|id| h.id != id));
            let after = k.effective().0;
            if after != before {
                changes.apply.push((k.name, after));
            }
        }
        changes
    }

    /// Each rule with whether it is on.
    pub fn rules(&self) -> impl Iterator<Item = (&Rule, bool)> {
        self.slots.iter().map(|s| (&s.rule, s.enabled))
    }

    /// The user's settings for one rule: on or off, and declared params of the same type. Turning a running rule
    /// off ends its episode like an Undo; the next `step` applies new params.
    pub fn configure(
        &mut self,
        rule: &str,
        enabled: bool,
        params: &[(String, crate::expr::Value)],
        time_ms: u64,
    ) -> Result<Changes, String> {
        let i = self.slots.iter().position(|s| s.rule.id == rule).ok_or_else(|| format!("unknown rule {rule:?}"))?;
        let slot = &mut self.slots[i];
        for (name, value) in params {
            let same_type = matches!(
                (slot.rule.params.get(name), value),
                (Some(crate::expr::Value::Num(_)), crate::expr::Value::Num(_))
                    | (Some(crate::expr::Value::Bool(_)), crate::expr::Value::Bool(_))
            );
            if !same_type {
                return Err(format!("{rule} has no {name} of this type"));
            }
        }
        for (name, value) in params {
            slot.rule.params.insert(name.clone(), value.clone());
        }
        let mut changes = Changes::default();
        if !enabled && let Some(event) = slot.episode.undo() {
            self.apply(i, event, Vec::new(), "turned off".to_owned(), time_ms, &mut changes);
        }
        if !enabled {
            let slot = &mut self.slots[i];
            for (instance, (episode, id)) in std::mem::take(&mut slot.instances) {
                if episode.active() {
                    let entry = instance_entry(
                        &slot.rule,
                        id,
                        Event::Ended(End::Undone),
                        &instance,
                        Vec::new(),
                        "turned off",
                        time_ms,
                    );
                    changes.entries.push(entry);
                }
            }
        }
        let slot = &mut self.slots[i];
        if !enabled {
            slot.episode = Episode::default();
        }
        slot.enabled = enabled;
        Ok(changes)
    }

    /// The next time `step` must run with no state change.
    pub fn deadline(&self) -> Option<Instant> {
        self.slots
            .iter()
            .filter(|s| s.enabled)
            .flat_map(|s| {
                std::iter::once(s.episode.deadline(&s.rule))
                    .chain(s.instances.values().map(|(e, _)| e.deadline(&s.rule)))
            })
            .flatten()
            .min()
    }

    fn apply(
        &mut self,
        i: usize,
        event: Event,
        trigger: Vec<(String, String)>,
        mut note: String,
        time_ms: u64,
        changes: &mut Changes,
    ) {
        let slot = &mut self.slots[i];
        if event == Event::Started {
            slot.id = self.next_id;
            self.next_id += 1;
        }
        let (id, owner, priority) = (slot.id, slot.rule.id.clone(), slot.rule.priority);
        let mut moved = Vec::new();
        for (name, value) in &slot.holds {
            let Some(knob) = self.knobs.get_mut(name) else { continue };
            let before = knob.effective().0;
            match event {
                Event::Started => knob.holds.push(Hold {
                    id,
                    owner: owner.clone(),
                    priority,
                    value: value.clone(),
                    reason: owner.clone(),
                    since_ms: time_ms,
                }),
                Event::Ended(_) => knob.holds.retain(|h| h.id != id),
            }
            let (after, source) = knob.effective();
            if after == before {
                if let (Event::Started, Source::Holds(_, winners)) = (event, source)
                    && !winners.contains(&owner)
                {
                    note = format!("no effect: {}", winners.join(", "));
                }
            } else {
                moved.push((name.to_string(), text(before.as_ref()), text(after.as_ref())));
                changes.apply.retain(|(n, _)| n != name);
                changes.apply.push((*name, after));
            }
        }
        let name = match event {
            Event::Started => "started",
            Event::Ended(End::Cleared) => "cleared",
            Event::Ended(End::Undone) => "undone",
        };
        changes.entries.push(Entry {
            id: format!("{time_ms}-{id}-{name}"),
            time_ms,
            rule: owner,
            episode: id,
            event: name.to_owned(),
            trigger,
            holds: slot.holds.iter().map(|(k, v)| ((*k).to_owned(), v.to_string())).collect(),
            changes: moved,
            measured: None,
            note,
            instance: String::new(),
        });
    }

    /// Steps every per-app rule over the apps now present: `apps` gives each app's values (state plus `app.*`).
    /// An app that left ends its running episode.
    pub fn step_instances(&mut self, apps: &[(String, &dyn Values)], now: Instant, time_ms: u64) -> Changes {
        let mut changes = Changes::default();
        for slot in self.slots.iter_mut().filter(|s| s.enabled && s.rule.per.is_some()) {
            for (app, values) in apps {
                let (episode, id) = slot.instances.entry(app.clone()).or_insert((Episode::default(), 0));
                let Some(event) = episode.step(&slot.rule, *values, now) else { continue };
                if event == Event::Started {
                    *id = self.next_id;
                    self.next_id += 1;
                }
                let read = if event == Event::Started { &slot.rule.when } else { &slot.rule.clear };
                let state = RuleValues { state: *values, params: &slot.rule.params };
                let trigger = read.names().into_iter().map(|n| (n.to_owned(), value_text(state.value_of(n)))).collect();
                changes.entries.push(instance_entry(&slot.rule, *id, event, app, trigger, "", time_ms));
            }
            let gone: Vec<String> =
                slot.instances.keys().filter(|k| !apps.iter().any(|(a, _)| a == *k)).cloned().collect();
            for app in gone {
                if let Some((episode, id)) = slot.instances.remove(&app)
                    && episode.active()
                {
                    let entry = instance_entry(
                        &slot.rule,
                        id,
                        Event::Ended(End::Cleared),
                        &app,
                        Vec::new(),
                        "app ended",
                        time_ms,
                    );
                    changes.entries.push(entry);
                }
            }
        }
        changes
    }

    /// The per-app rule and app of a running per-app episode.
    pub fn instance_of(&self, episode: u64) -> Option<(&Rule, &str)> {
        self.slots.iter().find_map(|s| {
            s.instances.iter().find(|(_, (e, id))| *id == episode && e.active()).map(|(app, _)| (&s.rule, app.as_str()))
        })
    }

    fn undo_instance(&mut self, episode: u64, time_ms: u64) -> Option<Changes> {
        for slot in &mut self.slots {
            for (app, (state, id)) in &mut slot.instances {
                if *id == episode && state.active() {
                    let event = state.undo()?;
                    let entry = instance_entry(&slot.rule, *id, event, app, Vec::new(), "undo", time_ms);
                    return Some(Changes { entries: vec![entry], ..Changes::default() });
                }
            }
        }
        None
    }
}

fn instance_entry(
    rule: &Rule,
    episode: u64,
    event: Event,
    app: &str,
    trigger: Vec<(String, String)>,
    note: &str,
    time_ms: u64,
) -> Entry {
    let name = match event {
        Event::Started => "started",
        Event::Ended(End::Cleared) => "cleared",
        Event::Ended(End::Undone) => "undone",
    };
    Entry {
        id: format!("{time_ms}-{episode}-{name}"),
        time_ms,
        rule: rule.id.clone(),
        episode,
        event: name.to_owned(),
        trigger,
        holds: rule.holds.iter().map(|h| (h.knob.clone(), h.value.to_string())).collect(),
        changes: Vec::new(),
        measured: None,
        note: note.to_owned(),
        instance: app.to_owned(),
    }
}

#[cfg(test)]
#[allow(clippy::unwrap_used)]
mod tests {
    use std::collections::HashMap;
    use std::time::{Duration, Instant};

    use super::Engine;
    use crate::expr::{Schema, Type, Value, Values};
    use crate::holds::{Knob, KnobValue, Merge};
    use crate::rules::load;

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

    #[allow(clippy::unnecessary_wraps)] // compared with the Option the engine returns
    fn profile(p: &str) -> Option<KnobValue> {
        Some(KnobValue::Str(p.to_owned()))
    }

    const RULES: &str = r#"
[[rule]]
id = "power.low_power_plan"
scope = "system"
priority = "automatic"
params = { low_pct = 20 }
when = "power.on_battery and battery.percent <= low_pct"
clear = "not power.on_battery"
holds = [{ knob = "power.profile", value = "power-saver" }]

[[rule]]
id = "bad.knob"
scope = "system"
priority = "automatic"
when = "power.on_battery"
clear = "not power.on_battery"
holds = [{ knob = "power.nope", value = "x" }]
"#;

    fn engine() -> Engine {
        let rules = load(RULES, &at(false, 0.0)).unwrap().into_iter().map(Result::unwrap).collect();
        let (mut engine, refused) = Engine::new(vec![Knob::new("power.profile", Merge::LowestPower)], rules, 1);
        assert_eq!(refused.len(), 1);
        assert_eq!(refused[0].0, "bad.knob");
        engine.set_baseline("power.profile", KnobValue::Str("balanced".to_owned()), 0).unwrap();
        engine
    }

    #[test]
    fn an_episode_holds_the_knob_and_logs_start_and_clear() {
        let mut engine = engine();
        let t0 = Instant::now();
        let started = engine.step(&at(true, 18.0), t0, 1000);
        assert_eq!(started.apply, vec![("power.profile", profile("power-saver"))]);
        let entry = &started.entries[0];
        assert_eq!((entry.rule.as_str(), entry.event.as_str()), ("power.low_power_plan", "started"));
        assert_eq!(
            entry.trigger,
            vec![
                ("power.on_battery".to_owned(), "true".to_owned()),
                ("battery.percent".to_owned(), "18".to_owned()),
                ("low_pct".to_owned(), "20".to_owned()),
            ]
        );
        assert_eq!(entry.changes, vec![("power.profile".to_owned(), "balanced".to_owned(), "power-saver".to_owned())]);

        let cleared = engine.step(&at(false, 18.0), t0 + Duration::from_secs(60), 61_000);
        assert_eq!(cleared.apply, vec![("power.profile", profile("balanced"))]);
        assert_eq!(cleared.entries[0].event, "cleared");
        assert_eq!(cleared.entries[0].episode, entry.episode);
    }

    #[test]
    fn a_new_threshold_applies_and_turning_off_ends_the_episode() {
        let mut engine = engine();
        let t0 = Instant::now();
        engine.configure("power.low_power_plan", true, &[("low_pct".to_owned(), Value::Num(15.0))], 0).unwrap();
        assert!(engine.step(&at(true, 18.0), t0, 1000).apply.is_empty());
        assert_eq!(engine.step(&at(true, 14.0), t0, 2000).apply, vec![("power.profile", profile("power-saver"))]);
        let off = engine.configure("power.low_power_plan", false, &[], 3000).unwrap();
        assert_eq!(off.apply, vec![("power.profile", profile("balanced"))]);
        assert!(engine.step(&at(true, 10.0), t0, 4000).entries.is_empty());
        assert!(
            engine.configure("power.low_power_plan", true, &[("low_pct".to_owned(), Value::Bool(true))], 0).is_err()
        );
    }

    #[test]
    fn a_user_change_undoes_the_automatic_episode_and_wins() {
        let mut engine = engine();
        let t0 = Instant::now();
        let episode = engine.step(&at(true, 18.0), t0, 1000).entries[0].episode;
        let changes = engine.set_baseline("power.profile", KnobValue::Str("performance".to_owned()), 2000).unwrap();
        assert_eq!(changes.apply, vec![("power.profile", profile("performance"))]);
        assert_eq!(changes.entries[0].event, "undone");
        assert_eq!(changes.entries[0].note, "you changed power.profile");
        // Muted until the episode clears: still on battery and low, no new hold.
        assert!(engine.step(&at(true, 15.0), t0 + Duration::from_secs(30), 31_000).apply.is_empty());
        assert!(engine.undo(episode, 40_000).is_none());
    }

    struct App(f64, bool);

    impl Values for App {
        fn value_of(&self, name: &str) -> Option<Value> {
            match name {
                "app.cpu_pct" => Some(Value::Num(self.0)),
                "app.focused" => Some(Value::Bool(self.1)),
                _ => None,
            }
        }
    }

    impl Schema for App {
        fn type_of(&self, name: &str) -> Option<Type> {
            match name {
                "app.cpu_pct" => Some(Type::Num),
                "app.focused" => Some(Type::Bool),
                _ => None,
            }
        }
    }

    fn pair<'a>(a: &'a App, b: &'a App) -> Vec<(String, &'a dyn Values)> {
        rows(&[("a.scope", a), ("b.scope", b)])
    }

    fn rows<'a>(apps: &[(&str, &'a App)]) -> Vec<(String, &'a dyn Values)> {
        apps.iter().map(|(name, app)| ((*name).to_owned(), *app as &dyn Values)).collect()
    }

    #[test]
    fn per_app_episodes_start_per_app_end_when_the_app_leaves_and_undo_mutes() {
        let text = r#"
[[rule]]
id = "apps.drainer"
scope = "session"
priority = "automatic"
per = "app"
when = "app.cpu_pct >= 50 and not app.focused"
for = "2m"
clear = "app.cpu_pct < 20"
holds = [{ knob = "app.tier", value = "T3", when = "not app.focused" }]
"#;
        let rules = load(text, &App(0.0, false)).unwrap().into_iter().map(Result::unwrap).collect();
        let (mut engine, refused) = Engine::new(Vec::new(), rules, 1);
        assert!(refused.is_empty());
        let t0 = Instant::now();
        let (busy, focused, calm) = (App(80.0, false), App(90.0, true), App(5.0, false));
        assert!(engine.step_instances(&pair(&busy, &focused), t0, 0).entries.is_empty());
        let started = engine.step_instances(&pair(&busy, &focused), t0 + Duration::from_secs(121), 1).entries;
        assert_eq!(started.len(), 1, "only the busy app that is not focused starts");
        assert_eq!((started[0].instance.as_str(), started[0].event.as_str()), ("a.scope", "started"));
        assert_eq!(engine.instance_of(started[0].episode).map(|(_, app)| app), Some("a.scope"));

        let undone = engine.undo(started[0].episode, 2).unwrap().entries;
        assert_eq!((undone[0].event.as_str(), undone[0].instance.as_str()), ("undone", "a.scope"));
        let later = t0 + Duration::from_secs(400);
        assert!(
            engine.step_instances(&pair(&busy, &focused), later, 3).entries.is_empty(),
            "an undone episode stays muted"
        );

        engine.step_instances(&pair(&calm, &focused), later, 4);
        engine.step_instances(&pair(&busy, &focused), later, 5);
        let again = engine.step_instances(&pair(&busy, &focused), later + Duration::from_secs(121), 6).entries;
        assert_eq!(again.len(), 1, "after it cleared, the app can start a new episode");
        let gone = engine.step_instances(&rows(&[("b.scope", &focused)]), later + Duration::from_secs(130), 7).entries;
        assert_eq!((gone[0].event.as_str(), gone[0].note.as_str()), ("cleared", "app ended"));
    }
}
