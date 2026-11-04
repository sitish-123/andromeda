//! `/org/andromeda/System1/Storage`: free space of `/` and `/home` (when it is its own filesystem), read every
//! 10 min (docs/system-health.md §8 budgets).

use std::time::Duration;

use zbus::object_server::InterfaceRef;
use zbus::{Connection, interface};

use crate::manager;

pub const PATH: &str = "/org/andromeda/System1/Storage";
pub const AREA: &str = "storage";
const EVERY: Duration = Duration::from_secs(600);

/// (mount point, filesystem type, free bytes for users, size bytes)
pub type Mount = (String, String, u64, u64);

#[derive(Default)]
pub struct Storage {
    mounts: Vec<Mount>,
}

#[interface(name = "org.andromeda.System1.Storage")]
impl Storage {
    /// (mount point, filesystem type, free bytes for users, size bytes) for `/` and a separate `/home`.
    #[zbus(property)]
    fn mounts(&self) -> Vec<Mount> {
        self.mounts.clone()
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
        let available = !mounts.is_empty();
        let result: zbus::Result<()> = async {
            let iface: InterfaceRef<Storage> = conn.object_server().interface(PATH).await?;
            let changed = std::mem::replace(&mut iface.get_mut().await.mounts, mounts.clone()) != mounts;
            if changed {
                iface.get().await.mounts_changed(iface.signal_emitter()).await?;
            }
            manager::set_area(&conn, AREA, available, if available { "" } else { "statvfs failed" }).await
        }
        .await;
        if let Err(e) = result {
            log::warn!("storage: {e}");
        }
    }
}
