//! The explain log (docs/system-health.md §6): one JSON line per episode event, kept for 7 days and at most 1 MiB,
//! so Settings can list "What the desktop did" and undo an entry.

use std::collections::HashMap;
use std::fs::{self, OpenOptions};
use std::io::Write;
use std::path::PathBuf;

use serde::{Deserialize, Serialize};
use zbus::message::Header;
use zbus::object_server::SignalEmitter;
use zbus::{Connection, OwnedVariant, Variant, interface};

use crate::policy::{self, Shared};
use crate::polkit;

const KEEP_MS: u64 = 7 * 24 * 3600 * 1000;
const MAX_BYTES: usize = 1 << 20;

#[derive(Clone, Debug, Deserialize, PartialEq, Serialize)]
pub struct Entry {
    /// Unique per log: `<time_ms>-<rule>`.
    pub id: String,
    pub time_ms: u64,
    pub rule: String,
    /// Shared by the start entry and the end entry of one episode.
    pub episode: u64,
    /// started, cleared, undone, superseded, failed.
    pub event: String,
    /// The fields the rule's `when` read, as text, at the time of the event.
    pub trigger: Vec<(String, String)>,
    /// knob, value: the holds the episode placed.
    pub holds: Vec<(String, String)>,
    /// knob, value before, value after: what the holds changed after arbitration.
    pub changes: Vec<(String, String, String)>,
    /// The rule's `measure` result, on the end entry.
    #[serde(default)]
    pub measured: Option<String>,
    #[serde(default)]
    pub note: String,
    /// The app (unit) of a per-app rule's episode; empty for other rules.
    #[serde(default)]
    pub instance: String,
}

pub struct Log {
    path: PathBuf,
}

impl Log {
    pub fn new(path: PathBuf) -> Self {
        Self { path }
    }

    /// Appends; the file is trimmed when it passes the size cap.
    pub fn append(&self, entry: &Entry) -> std::io::Result<()> {
        if let Some(dir) = self.path.parent() {
            fs::create_dir_all(dir)?;
        }
        let line = serde_json::to_string(entry).map_err(std::io::Error::other)? + "\n";
        let mut file = OpenOptions::new().create(true).append(true).open(&self.path)?;
        file.write_all(line.as_bytes())?;
        if file.metadata()?.len() > MAX_BYTES as u64 {
            self.trim(entry.time_ms)?;
        }
        Ok(())
    }

    /// Entries at or after `since_ms`, oldest first. Lines that do not parse are skipped.
    pub fn since(&self, since_ms: u64) -> Vec<Entry> {
        let text = fs::read_to_string(&self.path).unwrap_or_default();
        text.lines().filter_map(|l| serde_json::from_str::<Entry>(l).ok()).filter(|e| e.time_ms >= since_ms).collect()
    }

    /// Drops entries older than 7 days, then the oldest until the file fits the cap; writes through a temp file.
    pub fn trim(&self, now_ms: u64) -> std::io::Result<()> {
        let mut lines: Vec<String> =
            self.since(now_ms.saturating_sub(KEEP_MS)).iter().filter_map(|e| serde_json::to_string(e).ok()).collect();
        let mut size: usize = lines.iter().map(|l| l.len() + 1).sum();
        let mut first = 0;
        while size > MAX_BYTES / 2 && first < lines.len() {
            size -= lines[first].len() + 1;
            first += 1;
        }
        lines.drain(..first);
        let tmp = self.path.with_extension("tmp");
        fs::write(&tmp, lines.iter().fold(String::new(), |out, l| out + l + "\n"))?;
        fs::rename(tmp, &self.path)
    }
}

pub const PATH: &str = "/org/andromeda/System1/Explain";

impl Entry {
    /// The entry as `a{sv}`: `id`, `time_usec`, `rule`, `episode`, `event`, `trigger` a(ss), `holds` a(ss),
    /// `changes` a(sss), `measured`, `note`, `instance`.
    pub fn to_dict(&self) -> HashMap<String, OwnedVariant> {
        let mut out = HashMap::new();
        let mut put = |key: &str, value: Variant<'_>| {
            if let Ok(v) = value.try_into() {
                out.insert(key.to_owned(), v);
            }
        };
        put("id", self.id.as_str().into());
        put("time_usec", (self.time_ms * 1000).into());
        put("rule", self.rule.as_str().into());
        put("episode", self.episode.into());
        put("event", self.event.as_str().into());
        put("trigger", self.trigger.clone().into());
        put("holds", self.holds.clone().into());
        put("changes", self.changes.clone().into());
        put("measured", self.measured.clone().unwrap_or_default().into());
        put("note", self.note.as_str().into());
        put("instance", self.instance.as_str().into());
        out
    }
}

pub struct Explain {
    pub policy: Shared,
}

#[interface(name = "org.andromeda.System1.Explain")]
impl Explain {
    /// Entries at or after `since_usec` (wall clock), oldest first.
    async fn list(&self, since_usec: u64) -> Vec<HashMap<String, OwnedVariant>> {
        self.policy.lock().await.log.since(since_usec / 1000).iter().map(Entry::to_dict).collect()
    }

    /// Undoes the episode of `entry`: its holds end and the rule stays quiet until the episode clears.
    async fn undo(
        &self,
        entry: &str,
        #[zbus(header)] header: Header<'_>,
        #[zbus(connection)] conn: &Connection,
    ) -> zbus::fdo::Result<()> {
        polkit::check(conn, &header, "org.andromeda.system.undo").await?;
        let mut policy = self.policy.lock().await;
        let episode = policy.log.since(0).into_iter().find(|e| e.id == entry).map(|e| e.episode);
        let episode = episode.ok_or_else(|| zbus::fdo::Error::InvalidArgs(format!("no entry {entry:?}")))?;
        let changes = policy
            .engine
            .undo(episode, policy::now_ms())
            .ok_or_else(|| zbus::fdo::Error::Failed("the episode already ended".into()))?;
        policy.commit(conn, changes, None).await.map_err(|e| zbus::fdo::Error::Failed(format!("{e:#}")))
    }

    #[zbus(signal)]
    pub async fn entry(emitter: &SignalEmitter<'_>, entry: HashMap<String, OwnedVariant>) -> zbus::Result<()>;
}

#[cfg(test)]
#[allow(clippy::unwrap_used)]
mod tests {
    use super::{Entry, KEEP_MS, Log};

    fn entry(time_ms: u64, rule: &str) -> Entry {
        Entry {
            id: format!("{time_ms}-{rule}"),
            time_ms,
            rule: rule.to_owned(),
            episode: 1,
            event: "started".to_owned(),
            trigger: vec![("battery.percent".to_owned(), "18".to_owned())],
            holds: vec![("power.profile".to_owned(), "power-saver".to_owned())],
            changes: vec![("power.profile".to_owned(), "balanced".to_owned(), "power-saver".to_owned())],
            measured: None,
            note: String::new(),
            instance: String::new(),
        }
    }

    #[test]
    fn trim_drops_old_entries_and_keeps_recent_ones() {
        let dir = std::env::temp_dir().join(format!("explain-test-{}", std::process::id()));
        let log = Log::new(dir.join("explain.jsonl"));
        let now = 100 * KEEP_MS;
        log.append(&entry(now - KEEP_MS - 1, "old")).unwrap();
        log.append(&entry(now - 1000, "recent")).unwrap();
        log.trim(now).unwrap();
        let rules: Vec<String> = log.since(0).into_iter().map(|e| e.rule).collect();
        assert_eq!(rules, vec!["recent".to_owned()]);
        std::fs::remove_dir_all(dir).unwrap();
    }
}
