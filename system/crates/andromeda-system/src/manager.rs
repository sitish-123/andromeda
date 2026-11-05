//! `/org/andromeda/System1`: API version and which areas are available, with the reason when one is not.

use std::collections::{BTreeMap, BTreeSet, HashMap};
use std::sync::LazyLock;

use tokio::sync::watch;
use zbus::message::Header;
use zbus::{Connection, interface};

pub const PATH: &str = "/org/andromeda/System1";
pub const VERSION: u32 = 1;

/// Areas some client watches; expensive fields (RAPL energy, 1 s samples) run only while their area is in it.
static WATCHED: LazyLock<watch::Sender<BTreeSet<String>>> = LazyLock::new(|| watch::Sender::new(BTreeSet::new()));

pub fn watched() -> watch::Receiver<BTreeSet<String>> {
    WATCHED.subscribe()
}

#[derive(Default)]
pub struct Manager {
    areas: BTreeMap<String, (bool, String)>,
    /// watch id -> (caller's unique name, areas)
    watches: HashMap<u32, (String, Vec<String>)>,
    next_watch: u32,
}

impl Manager {
    fn publish_watched(&self) {
        let areas: BTreeSet<String> = self.watches.values().flat_map(|(_, areas)| areas.iter().cloned()).collect();
        WATCHED.send_if_modified(|current| std::mem::replace(current, areas.clone()) != areas);
    }
}

#[interface(name = "org.andromeda.System1.Manager")]
impl Manager {
    #[zbus(property(emits_changed_signal = "const"))]
    #[allow(clippy::unused_self)]
    fn version(&self) -> u32 {
        VERSION
    }

    /// area -> (available, reason); the reason names the backend and why, so Settings never shows a blank value.
    #[zbus(property)]
    fn areas(&self) -> BTreeMap<String, (bool, String)> {
        self.areas.clone()
    }

    /// Starts the expensive sampling of these areas (1 s) until `Unwatch` or until the caller leaves the bus.
    #[allow(clippy::needless_pass_by_value)] // zbus hands the header by value
    fn watch(
        &mut self,
        areas: Vec<String>,
        #[zbus(header)] header: Header<'_>,
        #[zbus(connection)] conn: &Connection,
    ) -> zbus::fdo::Result<u32> {
        let owner = header.sender().ok_or_else(|| zbus::fdo::Error::Failed("no sender".into()))?.to_string();
        self.next_watch = self.next_watch.wrapping_add(1);
        let id = self.next_watch;
        self.watches.insert(id, (owner.clone(), areas));
        self.publish_watched();
        let conn = conn.clone();
        tokio::spawn(async move {
            if crate::bus::vanished(&conn, &owner).await.is_ok()
                && let Ok(iface) = conn.object_server().interface::<_, Manager>(PATH).await
            {
                let mut manager = iface.get_mut().await;
                manager.watches.retain(|_, (o, _)| *o != owner);
                manager.publish_watched();
            }
        });
        Ok(id)
    }

    fn unwatch(&mut self, id: u32) {
        self.watches.remove(&id);
        self.publish_watched();
    }
}

/// Records an area's availability and emits the change only when it differs.
pub async fn set_area(conn: &Connection, area: &str, available: bool, reason: &str) -> zbus::Result<()> {
    let iface = conn.object_server().interface::<_, Manager>(PATH).await?;
    let changed = {
        let mut manager = iface.get_mut().await;
        let next = (available, reason.to_owned());
        manager.areas.insert(area.to_owned(), next.clone()) != Some(next)
    };
    if changed {
        iface.get().await.areas_changed(iface.signal_emitter()).await?;
    }
    Ok(())
}
