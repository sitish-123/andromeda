//! Failed systemd units for `per = "unit"` rules, from the system and the user manager.

use zbus::Connection;

const SYSTEMD: &str = "org.freedesktop.systemd1";
const MANAGER_PATH: &str = "/org/freedesktop/systemd1";
const MANAGER: &str = "org.freedesktop.systemd1.Manager";

/// One row of `ListUnitsFiltered`.
type UnitRow =
    (String, String, String, String, String, String, zbus::OwnedObjectPath, u32, String, zbus::OwnedObjectPath);

/// Whether a failed unit counts as a failed service: apps launched as units (`app-*`) exit with errors as apps do.
pub fn counts(unit: &str) -> bool {
    !unit.starts_with("app-")
}

/// The failed units of one manager; user units get a `user ` prefix, so both managers fit one instance list.
pub async fn failed(conn: &Connection, user: bool) -> zbus::Result<Vec<String>> {
    let reply =
        conn.call_method(Some(SYSTEMD), MANAGER_PATH, Some(MANAGER), "ListUnitsFiltered", &(vec!["failed"],)).await?;
    let rows: Vec<UnitRow> = reply.body().deserialize()?;
    let prefix = if user { "user " } else { "" };
    Ok(rows.into_iter().map(|r| r.0).filter(|u| counts(u)).map(|u| format!("{prefix}{u}")).collect())
}
