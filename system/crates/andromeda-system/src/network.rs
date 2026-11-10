//! `/org/andromeda/System1/Network`: connectivity, metered, VPN, and the hotspot's access-point interface (nl80211).
//! Stage 1 backend: NetworkManager.

use std::collections::HashMap;
use std::sync::{Arc, Mutex, MutexGuard};

use anyhow::{Result, anyhow};
use zbus::message::Header;
use zbus::object_server::InterfaceRef;
use zbus::{Connection, OwnedObjectPath, interface};

use crate::bus::{self, Remote};
use crate::{manager, polkit, wifi};

pub const PATH: &str = "/org/andromeda/System1/Network";
pub const AREA: &str = "network";
const NM: &str = "org.freedesktop.NetworkManager";
const NM_PATH: &str = "/org/freedesktop/NetworkManager";
const ACTIVE_IFACE: &str = "org.freedesktop.NetworkManager.Connection.Active";

/// `NM_CONNECTIVITY_*`: 0 unknown, 1 none, 2 portal, 3 limited, 4 full.
const CONNECTIVITY: [&str; 5] = ["unknown", "none", "portal", "limited", "full"];

#[derive(Default, Clone, PartialEq)]
pub struct NetworkData {
    pub connectivity: String,
    pub metered: bool,
    pub vpn: bool,
}

const HOTSPOT_ACTION: &str = "org.andromeda.system.wifi-hotspot";

/// Access-point interface name to the unique bus name that created it; memory only, so a restart forgets it.
type Creators = Arc<Mutex<HashMap<String, String>>>;

#[derive(Default)]
pub struct Network {
    data: NetworkData,
    creators: Creators,
}

fn lock(creators: &Creators) -> MutexGuard<'_, HashMap<String, String>> {
    creators.lock().unwrap_or_else(std::sync::PoisonError::into_inner)
}

/// Removes access-point interface `ap` when `creator` leaves the bus and still owns it (a shell crash or logout).
async fn remove_when_gone(conn: Connection, creators: Creators, creator: String, ap: String) {
    if let Err(e) = bus::vanished(&conn, &creator).await {
        log::warn!("network: cannot follow {creator}, {ap} stays: {e:#}");
        return;
    }
    {
        let mut owners = lock(&creators);
        if owners.get(&ap) != Some(&creator) {
            return;
        }
        owners.remove(&ap);
    }
    let name = ap.clone();
    match blocking(move || wifi::remove_ap_interface(&name)).await {
        Ok(wifi::ApRemoval::Removed) => log::info!("network: removed access-point interface {ap}, {creator} left"),
        Ok(_) => {}
        Err(e) => log::warn!("network: {ap} not removed after {creator} left: {e}"),
    }
}

async fn blocking<T: Send + 'static>(f: impl FnOnce() -> Result<T> + Send + 'static) -> zbus::fdo::Result<T> {
    tokio::task::spawn_blocking(f)
        .await
        .map_err(|e| zbus::fdo::Error::Failed(e.to_string()))?
        .map_err(|e| zbus::fdo::Error::Failed(format!("{e:#}")))
}

#[interface(name = "org.andromeda.System1.Network")]
impl Network {
    /// Whether the card of Wi-Fi interface `client` can run a hotspot while `client` stays connected.
    async fn can_run_ap_with_client(&self, client: String) -> zbus::fdo::Result<bool> {
        blocking(move || wifi::can_run_ap_with_client(&client)).await
    }

    /// Adds an access-point interface beside Wi-Fi interface `client` on the same card and returns its name. Calling
    /// it again returns the same interface while it exists. The interface goes when its caller leaves the bus.
    async fn create_ap_interface(
        &self,
        client: String,
        #[zbus(header)] header: Header<'_>,
        #[zbus(connection)] conn: &Connection,
    ) -> zbus::fdo::Result<String> {
        polkit::check(conn, &header, HOTSPOT_ACTION).await?;
        let (name, _) = blocking(move || wifi::create_ap_interface(&client)).await?;
        log::info!("network: access-point interface {name} ready");
        if let Some(sender) = header.sender().map(ToString::to_string) {
            let old = lock(&self.creators).insert(name.clone(), sender.clone());
            if old.as_ref() != Some(&sender) {
                tokio::spawn(remove_when_gone(conn.clone(), Arc::clone(&self.creators), sender, name.clone()));
            }
        }
        Ok(name)
    }

    /// Deletes access-point interface `ap`; only one of the kind `CreateApInterface` makes, also from before a
    /// restart of this service.
    async fn remove_ap_interface(
        &self,
        ap: String,
        #[zbus(header)] header: Header<'_>,
        #[zbus(connection)] conn: &Connection,
    ) -> zbus::fdo::Result<()> {
        polkit::check(conn, &header, HOTSPOT_ACTION).await?;
        let name = ap.clone();
        match blocking(move || wifi::remove_ap_interface(&name)).await? {
            wifi::ApRemoval::NotOurs => {
                return Err(zbus::fdo::Error::AccessDenied(format!("{ap} is not a hotspot interface of this service")));
            }
            wifi::ApRemoval::Removed => log::info!("network: removed access-point interface {ap}"),
            wifi::ApRemoval::Absent => {}
        }
        lock(&self.creators).remove(&ap);
        Ok(())
    }


    /// unknown, none, portal, limited or full.
    #[zbus(property)]
    fn connectivity(&self) -> String {
        self.data.connectivity.clone()
    }
    /// The primary connection is metered (set by the user or guessed by NetworkManager).
    #[zbus(property)]
    fn metered(&self) -> bool {
        self.data.metered
    }
    /// A VPN or WireGuard connection is active.
    #[zbus(property)]
    fn vpn(&self) -> bool {
        self.data.vpn
    }
}

async fn update(conn: &Connection, data: NetworkData) -> zbus::Result<()> {
    let iface: InterfaceRef<Network> = conn.object_server().interface(PATH).await?;
    let old = std::mem::replace(&mut iface.get_mut().await.data, data.clone());
    let network = iface.get().await;
    let e = iface.signal_emitter();
    if old.connectivity != data.connectivity {
        network.connectivity_changed(e).await?;
    }
    if old.metered != data.metered {
        network.metered_changed(e).await?;
    }
    if old.vpn != data.vpn {
        network.vpn_changed(e).await?;
    }
    Ok(())
}

async fn any_vpn(conn: &Connection, active: &[OwnedObjectPath]) -> bool {
    for path in active {
        let Ok(remote) = Remote::new(conn, NM, path.to_string(), ACTIVE_IFACE).await else { continue };
        let kind: String = bus::get(&remote.props, "Type").unwrap_or_default();
        if bus::get::<bool>(&remote.props, "Vpn").unwrap_or_default() || kind == "wireguard" {
            return true;
        }
    }
    false
}

async fn read(conn: &Connection, props: &bus::Props) -> NetworkData {
    let connectivity = bus::get::<u32>(props, "Connectivity").and_then(|c| CONNECTIVITY.get(c as usize).copied());
    // NM_METERED_*: 1 yes, 3 guess-yes.
    let metered = matches!(bus::get::<u32>(props, "Metered"), Some(1 | 3));
    let active: Vec<OwnedObjectPath> = bus::get(props, "ActiveConnections").unwrap_or_default();
    NetworkData {
        connectivity: connectivity.unwrap_or("unknown").to_owned(),
        metered,
        vpn: any_vpn(conn, &active).await,
    }
}

pub async fn run(conn: Connection) {
    let conn = &conn;
    bus::supervise(conn, NM, AREA, move || follow(conn), move || update(conn, NetworkData::default())).await;
}

async fn follow(conn: &Connection) -> Result<()> {
    let gone = bus::vanished(conn, NM);
    let mut remote = Remote::new(conn, NM, NM_PATH.to_owned(), NM).await?;
    update(conn, read(conn, &remote.props).await).await?;
    manager::set_area(conn, AREA, true, "").await?;
    tokio::pin!(gone);
    loop {
        tokio::select! {
            result = &mut gone => return result,
            change = remote.next() => match change {
                Some(Ok(())) => update(conn, read(conn, &remote.props).await).await?,
                Some(Err(e)) => return Err(e),
                None => return Err(anyhow!("signal stream ended")),
            },
        }
    }
}
