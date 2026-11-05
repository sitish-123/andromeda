//! Crashes for `per = "crash"` rules: systemd-coredump journal entries the user may read (their own; every user's
//! when in `wheel` or `systemd-journal`).

use std::collections::VecDeque;
use std::time::{Duration, Instant};

/// `MESSAGE_ID` of a systemd-coredump entry.
pub const COREDUMP_ID: &str = "fc2e22bc6ee647b6b90729ab34a250b1";
/// How long one crash counts in `crash.count`.
pub const WINDOW: Duration = Duration::from_secs(24 * 3600);

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
        self.0.push_back((exe, at));
    }

    /// Executable -> crashes in the window at `now`; older ones are dropped.
    pub fn counts(&mut self, now: Instant) -> Vec<(String, usize)> {
        while self.0.front().is_some_and(|(_, at)| now.duration_since(*at) >= WINDOW) {
            self.0.pop_front();
        }
        let mut counts: Vec<(String, usize)> = Vec::new();
        for (exe, _) in &self.0 {
            match counts.iter_mut().find(|(e, _)| e == exe) {
                Some((_, n)) => *n += 1,
                None => counts.push((exe.clone(), 1)),
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
        assert_eq!(recent.counts(t0 + Duration::from_secs(61)), [("/a".to_owned(), 2)]);
        assert_eq!(recent.counts(t0 + WINDOW + Duration::from_secs(1)), [("/a".to_owned(), 1)]);
    }
}
