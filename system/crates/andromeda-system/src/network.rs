//! `/org/andromeda/System1/Network`: connectivity, metered, VPN, and the hotspot's access-point interface
//! (`wpa_supplicant`, nl80211). Stage 1 backend: NetworkManager.

use std::collections::HashMap;
use std::path::PathBuf;
use std::sync::{Arc, Mutex, MutexGuard};
use std::time::Duration;

use anyhow::{Result, anyhow, bail};
use zbus::message::Header;
use zbus::object_server::InterfaceRef;
use zbus::{Connection, OwnedObjectPath, OwnedVariant, Variant, interface};

use crate::bus::{self, Remote};
use crate::{manager, polkit, wifi};

pub const PATH: &str = "/org/andromeda/System1/Network";
pub const AREA: &str = "network";
const NM: &str = "org.freedesktop.NetworkManager";
const NM_PATH: &str = "/org/freedesktop/NetworkManager";
const ACTIVE_IFACE: &str = "org.freedesktop.NetworkManager.Connection.Active";
const DEVICE_IFACE: &str = "org.freedesktop.NetworkManager.Device";

/// `NM_CONNECTIVITY_*`: 0 unknown, 1 none, 2 portal, 3 limited, 4 full.
const CONNECTIVITY: [&str; 5] = ["unknown", "none", "portal", "limited", "full"];

#[derive(Default, Clone, PartialEq)]
pub struct NetworkData {
    pub connectivity: String,
    pub metered: bool,
    pub vpn: bool,
}

const HOTSPOT_ACTION: &str = "org.andromeda.system.wifi-hotspot";

const WPA: &str = "fi.w1.wpa_supplicant1";
const WPA_PATH: &str = "/fi/w1/wpa_supplicant1";
/// `NM_DEVICE_STATE_DISCONNECTED`: NetworkManager has taken the interface over and set it to station mode.
const NM_DEVICE_DISCONNECTED: u32 = 30;
const NM_DEVICE_WAIT: Duration = Duration::from_secs(10);

/// Access-point interface name to the unique bus name that created it; memory only, so a restart forgets it.
type Creators = Arc<Mutex<HashMap<String, String>>>;

pub struct Network {
    data: NetworkData,
    creators: Creators,
    records: Arc<wifi::Records>,
}

impl Network {
    /// `records`: the file in the `StateDirectory` that lists the access-point interfaces this service made.
    pub fn new(records: PathBuf) -> Self {
        Self {
            data: NetworkData::default(),
            creators: Creators::default(),
            records: Arc::new(wifi::Records::new(records)),
        }
    }
}

fn lock(creators: &Creators) -> MutexGuard<'_, HashMap<String, String>> {
    creators.lock().unwrap_or_else(std::sync::PoisonError::into_inner)
}

/// Removes access-point interface `ap` when `creator` leaves the bus and still owns it (a shell crash or logout).
async fn remove_when_gone(
    conn: Connection,
    creators: Creators,
    records: Arc<wifi::Records>,
    creator: String,
    ap: String,
) {
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
    match remove_ap(&conn, records, &ap).await {
        Ok(wifi::Ownership::Ours(_)) => log::info!("network: removed access-point interface {ap}, {creator} left"),
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

async fn has_owner(conn: &Connection, name: &str) -> bool {
    let reply = conn
        .call_method(
            Some("org.freedesktop.DBus"),
            "/org/freedesktop/DBus",
            Some("org.freedesktop.DBus"),
            "NameHasOwner",
            &(name,),
        )
        .await;
    reply.is_ok_and(|reply| reply.body().deserialize::<bool>().unwrap_or(false))
}

async fn wpa_call<B: serde::Serialize + zbus::DynamicType>(
    conn: &Connection,
    method: &str,
    body: &B,
) -> zbus::Result<zbus::Message> {
    conn.call_method(Some(WPA), WPA_PATH, Some(WPA), method, body).await
}

/// `wpa_supplicant` makes the interface itself (`if_dynamic`): it keeps managing it, so NetworkManager adopts it
/// instead of failing to bring up a second station.
async fn wpa_create(conn: &Connection, name: &str) -> Result<()> {
    let args: HashMap<&str, Variant<'_>> = HashMap::from([
        ("Ifname", Variant::from(name)),
        ("Create", Variant::from(true)),
        ("Type", Variant::from("ap")),
    ]);
    wpa_call(conn, "CreateInterface", &(args,))
        .await
        .map_err(|e| anyhow!("wpa_supplicant cannot create {name}: {e}"))?;
    Ok(())
}

/// Has `wpa_supplicant` delete `name`; does nothing when `wpa_supplicant` does not manage it.
async fn wpa_remove(conn: &Connection, name: &str) -> Result<()> {
    let Ok(reply) = wpa_call(conn, "GetInterface", &(name,)).await else { return Ok(()) };
    let path: OwnedObjectPath = reply.body().deserialize()?;
    wpa_call(conn, "RemoveInterface", &(path,)).await?;
    Ok(())
}

/// Waits until NetworkManager has taken interface `name` over, so it no longer resets the interface to station mode
/// on its own. NetworkManager's device signals need a per-device subscription, so this polls for at most 10 s.
async fn wait_nm_device(conn: &Connection, name: &str) -> Result<()> {
    if !has_owner(conn, NM).await {
        return Ok(());
    }
    let deadline = tokio::time::Instant::now() + NM_DEVICE_WAIT;
    loop {
        if let Ok(reply) = conn.call_method(Some(NM), NM_PATH, Some(NM), "GetDeviceByIpIface", &(name,)).await {
            let path: OwnedObjectPath = reply.body().deserialize()?;
            let props = "org.freedesktop.DBus.Properties";
            let reply = conn.call_method(Some(NM), path.as_str(), Some(props), "Get", &(DEVICE_IFACE, "State")).await?;
            let state = u32::try_from(reply.body().deserialize::<OwnedVariant>()?)?;
            if state >= NM_DEVICE_DISCONNECTED {
                return Ok(());
            }
        }
        if tokio::time::Instant::now() >= deadline {
            bail!("NetworkManager did not take over {name} within {} s", NM_DEVICE_WAIT.as_secs());
        }
        tokio::time::sleep(Duration::from_millis(100)).await;
    }
}

/// Makes or re-arms the access-point interface beside `client`; returns its name.
async fn create_ap(conn: &Connection, records: Arc<wifi::Records>, client: String) -> zbus::fdo::Result<String> {
    let r = Arc::clone(&records);
    let plan = blocking(move || wifi::plan_ap(&*wifi::kernel()?, &r, &client)).await?;
    let name = plan.name.clone();
    if !plan.exists {
        if has_owner(conn, WPA).await {
            wpa_create(conn, &name).await.map_err(|e| zbus::fdo::Error::Failed(format!("{e:#}")))?;
        } else {
            let (n, wiphy) = (name.clone(), plan.wiphy);
            blocking(move || wifi::kernel()?.new_ap(wiphy, &n)).await?;
        }
    }
    let ready = async {
        let (r, n, wiphy) = (Arc::clone(&records), name.clone(), plan.wiphy);
        let ifindex = blocking(move || wifi::adopt_ap(&*wifi::kernel()?, &r, &n, wiphy)).await?;
        wait_nm_device(conn, &name).await.map_err(|e| zbus::fdo::Error::Failed(format!("{e:#}")))?;
        blocking(move || wifi::arm(&*wifi::kernel()?, ifindex)).await?;
        Ok::<_, zbus::fdo::Error>(ifindex)
    };
    match ready.await {
        Ok(_) => Ok(name),
        Err(e) => {
            // A re-armed interface stays: its caller removes it. A new one goes now; its caller never had it.
            if !plan.exists {
                let n = name.clone();
                if let Some(ifindex) = blocking(move || Ok(wifi::kernel()?.ifindex(&n))).await.ok().flatten()
                    && let Err(cleanup) = delete_ap(conn, Arc::clone(&records), &name, ifindex).await
                {
                    log::warn!("network: {name} stays after a failed start: {cleanup}");
                }
            }
            Err(e)
        }
    }
}

/// Deletes `ap` (`ifindex`) through `wpa_supplicant`, then nl80211 if it is still there, and forgets it.
async fn delete_ap(conn: &Connection, records: Arc<wifi::Records>, ap: &str, ifindex: u32) -> zbus::fdo::Result<()> {
    if has_owner(conn, WPA).await
        && let Err(e) = wpa_remove(conn, ap).await
    {
        log::warn!("network: wpa_supplicant did not remove {ap}: {e:#}");
    }
    let ap = ap.to_owned();
    blocking(move || wifi::delete_ap(&*wifi::kernel()?, &records, &ap, ifindex)).await
}

/// Removes `ap` when this service made it; returns what it found.
async fn remove_ap(conn: &Connection, records: Arc<wifi::Records>, ap: &str) -> zbus::fdo::Result<wifi::Ownership> {
    let (r, n) = (Arc::clone(&records), ap.to_owned());
    let found = blocking(move || wifi::ownership_of(&*wifi::kernel()?, &r, &n)).await?;
    match found {
        wifi::Ownership::Ours(ifindex) => delete_ap(conn, records, ap, ifindex).await?,
        wifi::Ownership::Absent => {
            let ap = ap.to_owned();
            blocking(move || records.forget(&ap)).await?;
        }
        wifi::Ownership::NotOurs => {}
    }
    Ok(found)
}

#[interface(name = "org.andromeda.System1.Network")]
impl Network {
    /// Whether a hotspot can run beside Wi-Fi interface `client` while it stays connected: the card allows both at
    /// once and the client's current channel allows an access point.
    async fn can_run_ap_with_client(&self, client: String) -> zbus::fdo::Result<bool> {
        blocking(move || wifi::can_run_ap_with_client(&*wifi::kernel()?, &client)).await
    }

    /// Adds an access-point interface beside Wi-Fi interface `client` on the same card, ready for an access point,
    /// and returns its name. Calling it again makes the same interface an access point again (NetworkManager resets
    /// it to station mode after each deactivation). The interface goes when its caller leaves the bus.
    async fn create_ap_interface(
        &self,
        client: String,
        #[zbus(header)] header: Header<'_>,
        #[zbus(connection)] conn: &Connection,
    ) -> zbus::fdo::Result<String> {
        polkit::check(conn, &header, HOTSPOT_ACTION).await?;
        let name = create_ap(conn, Arc::clone(&self.records), client).await?;
        log::info!("network: access-point interface {name} ready");
        if let Some(sender) = header.sender().map(ToString::to_string) {
            let old = lock(&self.creators).insert(name.clone(), sender.clone());
            if old.as_ref() != Some(&sender) {
                let (creators, records) = (Arc::clone(&self.creators), Arc::clone(&self.records));
                tokio::spawn(remove_when_gone(conn.clone(), creators, records, sender, name.clone()));
            }
        }
        Ok(name)
    }

    /// Deletes access-point interface `ap`; only one that `CreateApInterface` made, also before a restart of this
    /// service.
    async fn remove_ap_interface(
        &self,
        ap: String,
        #[zbus(header)] header: Header<'_>,
        #[zbus(connection)] conn: &Connection,
    ) -> zbus::fdo::Result<()> {
        polkit::check(conn, &header, HOTSPOT_ACTION).await?;
        match remove_ap(conn, Arc::clone(&self.records), &ap).await? {
            wifi::Ownership::NotOurs => {
                return Err(zbus::fdo::Error::AccessDenied(format!("{ap} is not a hotspot interface of this service")));
            }
            wifi::Ownership::Ours(_) => log::info!("network: removed access-point interface {ap}"),
            wifi::Ownership::Absent => {}
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
