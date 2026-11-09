//! Crashes for `per = "crash"` rules: systemd-coredump journal entries the user may read (their own; every user's
//! when in `wheel` or `systemd-journal`).

use std::collections::VecDeque;
use std::sync::Mutex;
use std::time::{Duration, Instant};

/// `MESSAGE_ID` of a systemd-coredump entry.
pub const COREDUMP_ID: &str = "fc2e22bc6ee647b6b90729ab34a250b1";
/// How long one crash counts in `crash.count`.
pub const WINDOW: Duration = Duration::from_secs(24 * 3600);

/// A crash this long before to this long after a desktop (compositor) crash is one of its victims: apps lose their
/// Wayland socket and dump a few seconds later, or a little before the compositor's own dump is written.
pub const AFTERMATH_BEFORE: Duration = Duration::from_secs(5);
pub const AFTERMATH: Duration = Duration::from_secs(60);

/// When the compositor last crashed; units that fail right after it are its victims too (session.rs).
static DESKTOP_CRASHED_AT: Mutex<Option<Instant>> = Mutex::new(None);

pub fn desktop_crashed_at() -> Option<Instant> {
    *DESKTOP_CRASHED_AT.lock().unwrap_or_else(std::sync::PoisonError::into_inner)
}

/// The Andromeda compositor's executable, whatever its install prefix.
pub fn is_desktop(exe: &str) -> bool {
    exe.rsplit('/').next() == Some("andromeda")
}

/// The executable of one `journalctl -o json` coredump line: `COREDUMP_EXE`, else `COREDUMP_COMM`.
pub fn exe_of(line: &str) -> Option<String> {
    let entry: serde_json::Value = serde_json::from_str(line).ok()?;
    if entry.get("MESSAGE_ID")?.as_str()? != COREDUMP_ID {
        return None;
    }
    let field = |k: &str| entry.get(k).and_then(serde_json::Value::as_str).filter(|s| !s.is_empty());
    field("COREDUMP_EXE").or_else(|| field("COREDUMP_COMM")).map(str::to_owned)
}

/// Crashes of the last `WINDOW`, oldest first.
#[derive(Default)]
pub struct Recent(VecDeque<(String, Instant)>);

impl Recent {
    pub fn add(&mut self, exe: String, at: Instant) {
        if is_desktop(&exe) {
            *DESKTOP_CRASHED_AT.lock().unwrap_or_else(std::sync::PoisonError::into_inner) = Some(at);
        }
        self.0.push_back((exe, at));
    }

    /// Executable -> (crashes in the window at `now`, every one of them came with a desktop crash); older ones are
    /// dropped.
    pub fn counts(&mut self, now: Instant) -> Vec<(String, usize, bool)> {
        while self.0.front().is_some_and(|(_, at)| now.duration_since(*at) >= WINDOW) {
            self.0.pop_front();
        }
        let desktop: Vec<Instant> = self.0.iter().filter(|(e, _)| is_desktop(e)).map(|(_, at)| *at).collect();
        let victim = |exe: &str, at: Instant| {
            !is_desktop(exe)
                && desktop.iter().any(|d| {
                    at.checked_duration_since(*d).map_or_else(|| d.duration_since(at) <= AFTERMATH_BEFORE, |after| after <= AFTERMATH)
                })
        };
        let mut counts: Vec<(String, usize, bool)> = Vec::new();
        for (exe, at) in &self.0 {
            let with_desktop = victim(exe, *at);
            match counts.iter_mut().find(|(e, _, _)| e == exe) {
                Some((_, n, all)) => {
                    *n += 1;
                    *all &= with_desktop;
                }
                None => counts.push((exe.clone(), 1, with_desktop)),
            }
        }
        counts
    }
}

#[cfg(test)]
#[allow(clippy::unwrap_used)]
mod tests {
    use super::*;

    #[test]
    fn a_coredump_line_names_its_executable_and_other_lines_do_not() {
        let line = format!(r#"{{"MESSAGE_ID":"{COREDUMP_ID}","COREDUMP_EXE":"/usr/bin/foot","COREDUMP_COMM":"foot"}}"#);
        assert_eq!(exe_of(&line).as_deref(), Some("/usr/bin/foot"));
        assert_eq!(exe_of(r#"{"MESSAGE_ID":"x","COREDUMP_EXE":"/usr/bin/foot"}"#), None);
    }

    #[test]
    fn a_crash_counts_for_one_window() {
        let t0 = Instant::now();
        let mut recent = Recent::default();
        recent.add("/a".into(), t0);
        recent.add("/a".into(), t0 + Duration::from_secs(60));
        assert_eq!(recent.counts(t0 + Duration::from_secs(61)), [("/a".to_owned(), 2, false)]);
        assert_eq!(recent.counts(t0 + WINDOW + Duration::from_secs(1)), [("/a".to_owned(), 1, false)]);
    }

    #[test]
    fn apps_that_die_with_the_compositor_are_its_victims_and_later_crashes_are_not() {
        let t0 = Instant::now();
        let mut recent = Recent::default();
        recent.add("/usr/bin/andromeda".into(), t0);
        recent.add("/opt/zen-browser-bin/zen-bin".into(), t0 + Duration::from_secs(4));
        recent.add("/usr/bin/foot".into(), t0 + Duration::from_secs(600));
        let counts = recent.counts(t0 + Duration::from_secs(601));
        let with_desktop = |exe: &str| counts.iter().find(|(e, _, _)| e == exe).map(|(_, _, w)| *w);
        assert_eq!(with_desktop("/usr/bin/andromeda"), Some(false));
        assert_eq!(with_desktop("/opt/zen-browser-bin/zen-bin"), Some(true));
        assert_eq!(with_desktop("/usr/bin/foot"), Some(false));
    }
}
