//! `/org/andromeda/System1/Storage`: free space of `/` and `/home` (when it is its own filesystem), and drives whose
//! SMART data says they fail (udisks2), read every 10 min (docs/system-health.md §8 budgets, §12.3).

use std::collections::HashMap;
use std::time::Duration;

use zbus::object_server::InterfaceRef;
use zbus::{Connection, interface};
use zbus::{OwnedObjectPath, OwnedVariant};

use crate::manager;

pub const PATH: &str = "/org/andromeda/System1/Storage";
pub const AREA: &str = "storage";
const EVERY: Duration = Duration::from_secs(600);

/// (mount point, filesystem type, free bytes for users, size bytes)
pub type Mount = (String, String, u64, u64);

#[derive(Default)]
pub struct Storage {
    mounts: Vec<Mount>,
    failing: Vec<(String, String)>,
    fs_errors: Vec<(String, u64)>,
}

#[interface(name = "org.andromeda.System1.Storage")]
impl Storage {
    /// (mount point, filesystem type, free bytes for users, size bytes) for `/` and a separate `/home`.
    #[zbus(property)]
    fn mounts(&self) -> Vec<Mount> {
        self.mounts.clone()
    }

    /// (drive model, what fails) for each drive whose SMART data fails: an ATA `SmartFailing`, or an `NVMe` critical
    /// warning other than temperature (`spare`, `degraded`, `readonly`, `volatile_mem`, `pmr_readonly`).
    #[zbus(property)]
    fn failing_drives(&self) -> Vec<(String, String)> {
        self.failing.clone()
    }

    /// (mount point, errors) for `/` and `/home` when the file system counted errors: ext4 `errors_count`, btrfs
    /// device `error_stats` summed.
    #[zbus(property)]
    fn fs_errors(&self) -> Vec<(String, u64)> {
        self.fs_errors.clone()
    }
}

const UDISKS: &str = "org.freedesktop.UDisks2";
type Objects = HashMap<OwnedObjectPath, HashMap<String, HashMap<String, OwnedVariant>>>;

/// The failing drives among udisks2's objects.
fn failing_drives(objects: &Objects) -> Vec<(String, String)> {
    let mut out = Vec::new();
    for interfaces in objects.values() {
        let Some(drive) = interfaces.get("org.freedesktop.UDisks2.Drive") else { continue };
        let model = drive.get("Model").and_then(|v| String::try_from(v.try_clone().ok()?).ok()).unwrap_or_default();
        let ata = interfaces.get("org.freedesktop.UDisks2.Drive.Ata").and_then(|a| a.get("SmartFailing"));
        if ata.and_then(|v| bool::try_from(v).ok()) == Some(true) {
            out.push((model.clone(), "failing".to_owned()));
        }
        let nvme =
            interfaces.get("org.freedesktop.UDisks2.NVMe.Controller").and_then(|n| n.get("SmartCriticalWarning"));
        let warnings: Vec<String> = nvme
            .and_then(|v| Vec::<String>::try_from(v.try_clone().ok()?).ok())
            .unwrap_or_default()
            .into_iter()
            .filter(|w| w != "temperature")
            .collect();
        if !warnings.is_empty() {
            out.push((model, warnings.join(", ")));
        }
    }
    out.sort();
    out
}

async fn read_failing(system: &Connection) -> Vec<(String, String)> {
    let reply = system
        .call_method(
            Some(UDISKS),
            "/org/freedesktop/UDisks2",
            Some("org.freedesktop.DBus.ObjectManager"),
            "GetManagedObjects",
            &(),
        )
        .await;
    match reply.and_then(|r| r.body().deserialize::<Objects>()) {
        Ok(objects) => failing_drives(&objects),
        Err(_) => Vec::new(), // no udisks2: nothing known to fail
    }
}

/// The mount point, filesystem type and source of each line of /proc/self/mounts.
fn mount_table(text: &str) -> Vec<(String, String, String)> {
    text.lines()
        .filter_map(|line| {
            let mut fields = line.split_ascii_whitespace();
            let source = fields.next()?.to_owned();
            Some((fields.next()?.replace("\\040", " "), fields.next()?.to_owned(), source))
        })
        .collect()
}

/// The sum of `name value` lines (btrfs `error_stats`).
fn sum_stats(text: &str) -> u64 {
    text.lines().filter_map(|l| l.split_ascii_whitespace().nth(1)?.parse::<u64>().ok()).sum()
}

/// Errors the file system on `source` counted; None when it keeps no counter we read.
fn fs_error_count(fs: &str, source: &str) -> Option<u64> {
    let device = std::path::Path::new(source).file_name()?.to_str()?;
    match fs {
        "ext4" => std::fs::read_to_string(format!("/sys/fs/ext4/{device}/errors_count")).ok()?.trim().parse().ok(),
        "btrfs" => {
            let fsid = std::fs::read_dir("/sys/fs/btrfs").ok()?.filter_map(Result::ok).find(|e| {
                std::fs::read_dir(e.path().join("devices"))
                    .is_ok_and(|mut d| d.any(|x| x.is_ok_and(|x| x.file_name() == device)))
            })?;
            let devinfo = std::fs::read_dir(fsid.path().join("devinfo")).ok()?;
            Some(
                devinfo
                    .filter_map(Result::ok)
                    .map(|d| sum_stats(&std::fs::read_to_string(d.path().join("error_stats")).unwrap_or_default()))
                    .sum(),
            )
        }
        _ => None,
    }
}

fn read_fs_errors() -> Vec<(String, u64)> {
    let table = mount_table(&std::fs::read_to_string("/proc/self/mounts").unwrap_or_default());
    let mut out = Vec::new();
    for point in ["/", "/home"] {
        let Some((_, fs, source)) = table.iter().rev().find(|(p, _, _)| p == point) else { continue };
        if out.iter().any(|(_, _, s): &(String, u64, String)| s == source) {
            continue; // /home on the same file system as /
        }
        if let Some(n) = fs_error_count(fs, source).filter(|n| *n > 0) {
            out.push((point.to_owned(), n, source.clone()));
        }
    }
    out.into_iter().map(|(p, n, _)| (p, n)).collect()
}

fn read_mounts() -> Vec<Mount> {
    let table = mount_table(&std::fs::read_to_string("/proc/self/mounts").unwrap_or_default());
    let mut mounts = Vec::new();
    for point in ["/", "/home"] {
        // The last entry for a mount point is the one on top.
        let Some((_, fs, _)) = table.iter().rev().find(|(p, _, _)| p == point) else { continue };
        let Ok(st) = rustix::fs::statvfs(point) else { continue };
        mounts.push((point.to_owned(), fs.clone(), st.f_bavail * st.f_frsize, st.f_blocks * st.f_frsize));
    }
    mounts
}

pub async fn run(conn: Connection) {
    let mut tick = tokio::time::interval(EVERY);
    loop {
        tick.tick().await;
        let mounts = read_mounts();
        let failing = read_failing(&conn).await;
        let fs_errors = read_fs_errors();
        let available = !mounts.is_empty();
        let result: zbus::Result<()> = async {
            let iface: InterfaceRef<Storage> = conn.object_server().interface(PATH).await?;
            let changed = std::mem::replace(&mut iface.get_mut().await.mounts, mounts.clone()) != mounts;
            if changed {
                iface.get().await.mounts_changed(iface.signal_emitter()).await?;
            }
            if std::mem::replace(&mut iface.get_mut().await.failing, failing.clone()) != failing {
                iface.get().await.failing_drives_changed(iface.signal_emitter()).await?;
            }
            if std::mem::replace(&mut iface.get_mut().await.fs_errors, fs_errors.clone()) != fs_errors {
                iface.get().await.fs_errors_changed(iface.signal_emitter()).await?;
            }
            manager::set_area(&conn, AREA, available, if available { "" } else { "statvfs failed" }).await
        }
        .await;
        if let Err(e) = result {
            log::warn!("storage: {e}");
        }
    }
}

#[cfg(test)]
#[allow(clippy::unwrap_used)]
mod tests {
    use std::collections::HashMap;

    use zbus::{OwnedObjectPath, OwnedVariant, Variant};

    use super::{Objects, failing_drives, sum_stats};

    #[test]
    fn btrfs_error_stats_sum_every_counter() {
        let stats = "write_errs 2\nread_errs 0\nflush_errs 0\ncorruption_errs 5\ngeneration_errs 1\n";
        assert_eq!(sum_stats(stats), 8);
    }

    fn drive(model: &str, extra: (&str, &str, Variant<'static>)) -> HashMap<String, HashMap<String, OwnedVariant>> {
        let props = |k: &str, v: Variant<'static>| HashMap::from([(k.to_owned(), OwnedVariant::try_from(v).unwrap())]);
        HashMap::from([
            ("org.freedesktop.UDisks2.Drive".to_owned(), props("Model", Variant::from(model.to_owned()))),
            (extra.0.to_owned(), props(extra.1, extra.2)),
        ])
    }

    #[test]
    fn smart_failures_count_and_a_hot_nvme_does_not() {
        let path = |p: &str| OwnedObjectPath::try_from(format!("/org/freedesktop/UDisks2/drives/{p}")).unwrap();
        let nvme = "org.freedesktop.UDisks2.NVMe.Controller";
        let objects: Objects = HashMap::from([
            (path("a"), drive("SATA1", ("org.freedesktop.UDisks2.Drive.Ata", "SmartFailing", Variant::from(true)))),
            (path("b"), drive("SATA2", ("org.freedesktop.UDisks2.Drive.Ata", "SmartFailing", Variant::from(false)))),
            (path("c"), drive("NVME1", (nvme, "SmartCriticalWarning", Variant::from(vec!["spare", "temperature"])))),
            (path("d"), drive("NVME2", (nvme, "SmartCriticalWarning", Variant::from(vec!["temperature"])))),
        ]);
        assert_eq!(
            failing_drives(&objects),
            vec![("NVME1".to_owned(), "spare".to_owned()), ("SATA1".to_owned(), "failing".to_owned())]
        );
    }
}
