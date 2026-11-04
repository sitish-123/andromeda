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
                Ok(holds) => slots.push(Slot { rule, holds, episode: Episode::default(), id: 0 }),
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
            if let Some(event) = slot.episode.step(&slot.rule, values, now) {
                let read = match event {
                    Event::Started => &slot.rule.when,
                    Event::Ended(_) => &slot.rule.clear,
                };
                let state = RuleValues { state: values, params: &slot.rule.params };
                let trigger =
                    read.names().into_iter().map(|n| (n.to_owned(), value_text(state.value_of(n)))).collect();
                self.apply(i, event, trigger, String::new(), time_ms, &mut changes);
            }
        }
        changes
    }

    /// The user's Undo of an episode, from the explain log or a notification action. `None` when it is not running.
    pub fn undo(&mut self, episode: u64, time_ms: u64) -> Option<Changes> {
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

    /// The next time `step` must run with no state change.
    pub fn deadline(&self) -> Option<Instant> {
        self.slots.iter().filter_map(|s| s.episode.deadline(&s.rule)).min()
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
        });
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
        assert_eq!(entry.trigger, vec![
            ("power.on_battery".to_owned(), "true".to_owned()),
            ("battery.percent".to_owned(), "18".to_owned()),
            ("low_pct".to_owned(), "20".to_owned()),
        ]);
        assert_eq!(entry.changes, vec![("power.profile".to_owned(), "balanced".to_owned(), "power-saver".to_owned())]);

        let cleared = engine.step(&at(false, 18.0), t0 + Duration::from_secs(60), 61_000);
        assert_eq!(cleared.apply, vec![("power.profile", profile("balanced"))]);
        assert_eq!(cleared.entries[0].event, "cleared");
        assert_eq!(cleared.entries[0].episode, entry.episode);
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
}
