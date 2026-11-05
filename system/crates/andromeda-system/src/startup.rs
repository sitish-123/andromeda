//! Boot and login times for the `startup.*` fields. Boot = firmware + loader + kernel + userspace, as
//! systemd-analyze counts; login = logind session start to the user's `graphical-session.target`.
//! History: one JSON line per login, `startup.jsonl` in the session part's state directory.

use std::path::Path;
use std::time::Duration;

use serde::{Deserialize, Serialize};
use zbus::Connection;
use zbus::OwnedObjectPath;

/// Earlier logins the medians need, and the most they read.
const MIN_HISTORY: usize = 5;
const MAX_HISTORY: usize = 10;
/// Lines kept in the history file.
const KEEP: usize = 50;

#[derive(Serialize, Deserialize, Clone, Debug, PartialEq)]
pub struct Record {
    pub boot_id: String,
    pub session: String,
    pub boot_s: f64,
    pub login_s: Option<f64>,
}

/// This login, and the medians of the earlier boots and logins.
#[allow(clippy::struct_field_names)] // seconds, as the state fields
pub struct Times {
    pub boot_s: f64,
    pub login_s: Option<f64>,
    pub boot_median_s: Option<f64>,
    pub login_median_s: Option<f64>,
}

pub fn median(mut values: Vec<f64>) -> Option<f64> {
    if values.len() < MIN_HISTORY {
        return None;
    }
    values.sort_by(f64::total_cmp);
    let mid = values.len() / 2;
    Some(if values.len().is_multiple_of(2) { f64::midpoint(values[mid - 1], values[mid]) } else { values[mid] })
}

/// The medians from `history` (oldest first) for `now`: one boot value per earlier boot, one login value per earlier
/// login; the current boot and login are left out.
pub fn times(history: &[Record], now: &Record) -> Times {
    let mut boots: Vec<(&str, f64)> = Vec::new();
    for r in history.iter().rev().filter(|r| r.boot_id != now.boot_id) {
        if !boots.iter().any(|(id, _)| *id == r.boot_id) {
            boots.push((&r.boot_id, r.boot_s));
        }
    }
    let logins: Vec<f64> = history
        .iter()
        .rev()
        .filter(|r| (r.boot_id.as_str(), r.session.as_str()) != (now.boot_id.as_str(), now.session.as_str()))
        .filter_map(|r| r.login_s)
        .take(MAX_HISTORY)
        .collect();
    Times {
        boot_s: now.boot_s,
        login_s: now.login_s,
        boot_median_s: median(boots.into_iter().take(MAX_HISTORY).map(|(_, s)| s).collect()),
        login_median_s: median(logins),
    }
}

async fn u64_prop(conn: &Connection, dest: &str, path: &str, iface: &str, name: &str) -> zbus::Result<u64> {
    let proxy = zbus::Proxy::new(conn, dest.to_owned(), path.to_owned(), iface.to_owned()).await?;
    proxy.get_property::<u64>(name).await
}

#[allow(clippy::cast_precision_loss)] // microseconds as seconds
fn secs(us: u64) -> f64 {
    us as f64 / 1e6
}

/// Measures this boot and login. Waits up to 10 min for the boot to finish and the graphical session to start.
pub async fn measure(system: &Connection, session: &Connection) -> zbus::Result<Record> {
    const SD: &str = "org.freedesktop.systemd1";
    const MGR: &str = "org.freedesktop.systemd1.Manager";
    let mgr = "/org/freedesktop/systemd1";
    let mut finish = 0;
    for _ in 0..60 {
        finish = u64_prop(system, SD, mgr, MGR, "FinishTimestampMonotonic").await?;
        if finish > 0 {
            break;
        }
        tokio::time::sleep(Duration::from_secs(10)).await;
    }
    if finish == 0 {
        return Err(zbus::Error::Failure("the boot did not finish".into()));
    }
    let firmware = u64_prop(system, SD, mgr, MGR, "FirmwareTimestampMonotonic").await?;
    let login1 = zbus::Proxy::new(
        system,
        "org.freedesktop.login1",
        "/org/freedesktop/login1/user/self",
        "org.freedesktop.login1.User",
    )
    .await?;
    let (id, path): (String, OwnedObjectPath) = login1.get_property("Display").await?;
    let started = u64_prop(
        system,
        "org.freedesktop.login1",
        path.as_str(),
        "org.freedesktop.login1.Session",
        "TimestampMonotonic",
    )
    .await?;
    let target = session
        .call_method(Some(SD), mgr, Some(MGR), "GetUnit", &("graphical-session.target",))
        .await
        .and_then(|r| r.body().deserialize::<OwnedObjectPath>());
    let entered = match target {
        Ok(unit) => {
            u64_prop(session, SD, unit.as_str(), "org.freedesktop.systemd1.Unit", "ActiveEnterTimestampMonotonic")
                .await?
        }
        Err(_) => 0,
    };
    let boot_id = std::fs::read_to_string("/proc/sys/kernel/random/boot_id").unwrap_or_default().trim().to_owned();
    Ok(Record {
        boot_id,
        session: id,
        boot_s: secs(firmware + finish),
        login_s: (entered > started && started > 0).then(|| secs(entered - started)),
    })
}

/// Reads the history, adds `now` once, keeps the last `KEEP` lines, and returns the history before `now`.
pub fn record(path: &Path, now: &Record) -> std::io::Result<Vec<Record>> {
    let text = std::fs::read_to_string(path).unwrap_or_default();
    let mut history: Vec<Record> = text.lines().filter_map(|l| serde_json::from_str(l).ok()).collect();
    let before = history.clone();
    if !history.iter().any(|r| r.boot_id == now.boot_id && r.session == now.session) {
        history.push(now.clone());
        let skip = history.len().saturating_sub(KEEP);
        if let Some(dir) = path.parent() {
            std::fs::create_dir_all(dir)?;
        }
        let lines: Vec<String> = history.iter().skip(skip).filter_map(|r| serde_json::to_string(r).ok()).collect();
        std::fs::write(path, lines.join("\n") + "\n")?;
    }
    Ok(before)
}

#[cfg(test)]
#[allow(clippy::unwrap_used)]
mod tests {
    use super::*;

    fn rec(boot: &str, session: &str, boot_s: f64, login_s: f64) -> Record {
        Record { boot_id: boot.into(), session: session.into(), boot_s, login_s: Some(login_s) }
    }

    #[test]
    fn medians_count_each_earlier_boot_once_and_leave_out_this_login() {
        let mut history: Vec<Record> = (0..5).map(|i| rec(&format!("b{i}"), "1", 10.0 + f64::from(i), 2.0)).collect();
        history.push(rec("b4", "2", 10.0 + 4.0, 3.0));
        let now = rec("b9", "1", 30.0, 9.0);
        let t = times(&history, &now);
        assert_eq!(t.boot_median_s, Some(12.0));
        assert_eq!(t.login_median_s, Some(2.0));
        assert_eq!(times(&history[..4], &now).boot_median_s, None);
    }
}
