//! `/org/andromeda/System1/Network`: connectivity, metered, VPN. Stage 1 backend: NetworkManager.

use anyhow::{Result, anyhow};
use zbus::object_server::InterfaceRef;
use zbus::{Connection, OwnedObjectPath, interface};

use crate::bus::{self, Remote};
use crate::manager;

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

#[derive(Default)]
pub struct Network {
    data: NetworkData,
}

#[interface(name = "org.andromeda.System1.Network")]
impl Network {
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
