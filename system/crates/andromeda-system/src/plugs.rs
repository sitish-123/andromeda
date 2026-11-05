//! Plug history for `battery.optimized_charging` (docs/system-health.md §4.3): each charger plug and unplug, kept 30
//! days in the state directory, and the learned unplug time of day.

use std::io::Write;
use std::path::PathBuf;
use std::sync::{Mutex, OnceLock};

use serde::{Deserialize, Serialize};

const KEEP_S: u64 = 30 * 86_400;
const LEARN_AFTER_S: u64 = 14 * 86_400;
/// Only an unplug after a long charge (the overnight one) teaches the unplug time.
const LONG_CHARGE_S: u64 = 3 * 3600;
const MIN_UNPLUGS: usize = 7;

#[derive(Clone, Copy, Debug, Deserialize, PartialEq, Serialize)]
pub struct Event {
    /// Unix time, seconds.
    pub t: u64,
    pub plugged: bool,
}

struct Plugs {
    path: PathBuf,
    events: Vec<Event>,
}

static PLUGS: OnceLock<Mutex<Plugs>> = OnceLock::new();

/// Loads the history from `path`, drops events older than 30 days and writes the rest back.
pub fn init(path: PathBuf, now: u64) {
    let text = std::fs::read_to_string(&path).unwrap_or_default();
    let events: Vec<Event> =
        text.lines().filter_map(|l| serde_json::from_str::<Event>(l).ok()).filter(|e| e.t + KEEP_S >= now).collect();
    let lines: String = events.iter().filter_map(|e| serde_json::to_string(e).ok()).map(|l| l + "\n").collect();
    if let Err(e) = std::fs::write(&path, lines) {
        log::warn!("plug history {}: {e}", path.display());
    }
    let _ = PLUGS.set(Mutex::new(Plugs { path, events }));
}

/// Records a change of the power source; a repeat of the last state is ignored.
pub fn record(plugged: bool, now: u64) {
    let Some(plugs) = PLUGS.get() else { return };
    let Ok(mut plugs) = plugs.lock() else { return };
    if plugs.events.last().map(|e| e.plugged) == Some(plugged) {
        return;
    }
    let event = Event { t: now, plugged };
    plugs.events.push(event);
    let line = serde_json::to_string(&event).unwrap_or_default();
    let appended = std::fs::OpenOptions::new().create(true).append(true).open(&plugs.path);
    if let Err(e) = appended.and_then(|mut f| writeln!(f, "{line}")) {
        log::warn!("plug history {}: {e}", plugs.path.display());
    }
}

/// The learned unplug time, in minutes after midnight UTC, or `None` before 14 days of history.
pub fn learned(now: u64) -> Option<u32> {
    learned_from(&PLUGS.get()?.lock().ok()?.events, now)
}

fn learned_from(events: &[Event], now: u64) -> Option<u32> {
    if events.first().is_none_or(|e| e.t + LEARN_AFTER_S > now) {
        return None;
    }
    let mut minutes: Vec<u32> = events
        .windows(2)
        .filter(|w| w[0].plugged && !w[1].plugged && w[1].t - w[0].t >= LONG_CHARGE_S)
        .filter_map(|w| u32::try_from(w[1].t % 86_400 / 60).ok())
        .collect();
    if minutes.len() < MIN_UNPLUGS {
        return None;
    }
    minutes.sort_unstable();
    Some(minutes[minutes.len() / 2])
}

#[cfg(test)]
mod tests {
    use super::*;

    const DAY: u64 = 86_400;

    /// Plugged at 22:00, unplugged at `unplug_min` past midnight, for `days` days from day 1.
    fn nights(days: u64, unplug_min: impl Fn(u64) -> u64) -> Vec<Event> {
        (1..=days)
            .flat_map(|d| {
                [
                    Event { t: d * DAY - 2 * 3600, plugged: true },
                    Event { t: d * DAY + unplug_min(d) * 60, plugged: false },
                ]
            })
            .collect()
    }

    #[test]
    fn the_median_overnight_unplug_is_learned_after_two_weeks() {
        let events = nights(20, |d| if d % 5 == 0 { 600 } else { 450 + d % 3 });
        assert_eq!(learned_from(&events, 21 * DAY), Some(451));
    }

    #[test]
    fn nothing_is_learned_before_two_weeks_or_from_short_charges() {
        assert_eq!(learned_from(&nights(10, |_| 450), 11 * DAY), None);
        let short: Vec<Event> = (1..=20)
            .flat_map(|d| [Event { t: d * DAY, plugged: true }, Event { t: d * DAY + 1800, plugged: false }])
            .collect();
        assert_eq!(learned_from(&short, 21 * DAY), None);
    }
}
