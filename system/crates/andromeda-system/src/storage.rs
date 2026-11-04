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

/// The mount point and filesystem type of each line of /proc/self/mounts.
fn mount_table(text: &str) -> Vec<(String, String)> {
    text.lines()
        .filter_map(|line| {
            let mut fields = line.split_ascii_whitespace().skip(1);
            Some((fields.next()?.replace("\\040", " "), fields.next()?.to_owned()))
        })
        .collect()
}

fn read_mounts() -> Vec<Mount> {
    let table = mount_table(&std::fs::read_to_string("/proc/self/mounts").unwrap_or_default());
    let mut mounts = Vec::new();
    for point in ["/", "/home"] {
        // The last entry for a mount point is the one on top.
        let Some((_, fs)) = table.iter().rev().find(|(p, _)| p == point) else { continue };
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

    use super::{Objects, failing_drives};

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
