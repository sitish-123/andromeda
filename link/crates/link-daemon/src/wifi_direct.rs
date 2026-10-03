//! Joining a phone's Wi-Fi Direct group as its client through `NetworkManager`'s Wi-Fi P2P device, which runs beside
//! the desktop's own Wi-Fi connection instead of replacing it. The profile is volatile, like the hotspot's.

use std::collections::HashMap;
use std::time::{Duration, Instant};

use zbus::zvariant::{OwnedObjectPath, OwnedValue, Value};

use crate::hotspot::{self, Joined, NM, NM_PATH, text};

const P2P_IFACE: &str = "org.freedesktop.NetworkManager.Device.WifiP2P";
const DEVICE_TYPE_WIFI_P2P: u32 = 30;
/// The phone starts looking for the desktop at the same time; finding each other takes a few scan rounds.
const FIND_TIMEOUT: Duration = Duration::from_secs(20);
/// Lets the phone's own connect go out first. Its request then waits on ours, so Android needs no confirmation and
/// the phone's higher intent makes it the owner, serving DHCP; a request of ours that arrives first finds Android
/// idle, which answers the app's connect with BUSY, prompts, and leaves the desktop owning a group whose DHCP a host
/// firewall drops.
const PHONE_FIRST: Duration = Duration::from_secs(3);

/// How phones find this desktop over Wi-Fi Direct, when `NetworkManager` has a Wi-Fi P2P device: the host name iwd
/// shows, and the device address a phone matches when `wpa_supplicant` shows no name. `NetworkManager` leaves the P2P
/// device's own address empty; drivers with a P2P-device interface, iwlwifi among them, give it the card's permanent
/// address, which the Wi-Fi device reports.
pub async fn local_peer() -> Option<link_core::proto::message::WifiDirectReady> {
    let bus = zbus::Connection::system().await.ok()?;
    p2p_device(&bus).await.ok()??;
    let name = std::fs::read_to_string("/proc/sys/kernel/hostname").ok()?.trim().to_owned();
    if !(1..=link_core::proto::message::MAX_P2P_NAME_LEN).contains(&name.len()) {
        return None;
    }
    let address = async {
        let manager = zbus::Proxy::new(&bus, NM, NM_PATH, NM).await.ok()?;
        let wifi = hotspot::wifi_device(&bus, &manager).await.ok()?;
        let wireless = zbus::Proxy::new(&bus, NM, wifi.as_str(), hotspot::WIRELESS).await.ok()?;
        wireless.get_property::<String>("PermHwAddress").await.ok().map(|address| address.to_lowercase())
    };
    Some(link_core::proto::message::WifiDirectReady { name, address: address.await })
}

/// Finds the phone shown as `peer_name` and joins its group.
pub async fn join(peer_name: &str) -> Result<Joined, String> {
    let bus = zbus::Connection::system().await.map_err(text)?;
    let device = p2p_device(&bus).await?.ok_or_else(|| "no Wi-Fi P2P device".to_owned())?;
    let p2p = zbus::Proxy::new(&bus, NM, device.as_str(), P2P_IFACE).await.map_err(text)?;
    let options: HashMap<&str, Value<'_>> = HashMap::from([("timeout", Value::from(30_i32))]);
    p2p.call::<_, _, ()>("StartFind", &(options,)).await.map_err(text)?;
    let found = find(&bus, &p2p, peer_name).await;
    if let Err(error) = p2p.call::<_, _, ()>("StopFind", &()).await {
        log::debug!("stopping the Wi-Fi Direct find: {error}");
    }
    let (peer, hwaddr) = found?;
    tokio::time::sleep(PHONE_FIRST).await;
    let user = std::env::var("USER").unwrap_or_default();
    let settings: HashMap<&str, HashMap<&str, Value<'_>>> = HashMap::from([
        (
            "connection",
            HashMap::from([
                ("id", Value::from(format!("Umbriel Link: {peer_name}"))),
                ("type", Value::from("wifi-p2p")),
                ("autoconnect", Value::from(false)),
                ("permissions", Value::from(vec![format!("user:{user}")])),
            ]),
        ),
        ("wifi-p2p", HashMap::from([("peer", Value::from(hwaddr))])),
        ("ipv4", HashMap::from([("method", Value::from("auto"))])),
        ("ipv6", HashMap::from([("method", Value::from("ignore"))])),
    ]);
    let manager = zbus::Proxy::new(&bus, NM, NM_PATH, NM).await.map_err(text)?;
    let options: HashMap<&str, Value<'_>> = HashMap::from([("persist", Value::from("volatile"))]);
    let (_, active, _): (OwnedObjectPath, OwnedObjectPath, HashMap<String, OwnedValue>) =
        manager.call("AddAndActivateConnection2", &(settings, &device, &peer, options)).await.map_err(text)?;
    hotspot::activated(&bus, active).await
}

async fn p2p_device(bus: &zbus::Connection) -> Result<Option<OwnedObjectPath>, String> {
    let manager = zbus::Proxy::new(bus, NM, NM_PATH, NM).await.map_err(text)?;
    let devices: Vec<OwnedObjectPath> = manager.get_property("Devices").await.map_err(text)?;
    for path in devices {
        let device =
            zbus::Proxy::new(bus, NM, path.as_str(), "org.freedesktop.NetworkManager.Device").await.map_err(text)?;
        if device.get_property::<u32>("DeviceType").await.map_err(text)? == DEVICE_TYPE_WIFI_P2P {
            return Ok(Some(path));
        }
    }
    Ok(None)
}

/// The peer object and hardware address of the device named `name`, once a find turns it up.
async fn find(bus: &zbus::Connection, p2p: &zbus::Proxy<'_>, name: &str) -> Result<(OwnedObjectPath, String), String> {
    let deadline = Instant::now() + FIND_TIMEOUT;
    while Instant::now() < deadline {
        let peers: Vec<OwnedObjectPath> = p2p.get_property("Peers").await.map_err(text)?;
        for path in peers {
            let peer = zbus::Proxy::new(bus, NM, path.as_str(), "org.freedesktop.NetworkManager.WifiP2PPeer")
                .await
                .map_err(text)?;
            if peer.get_property::<String>("Name").await.map_err(text)? == name {
                let hwaddr = peer.get_property::<String>("HwAddress").await.map_err(text)?;
                return Ok((path, hwaddr));
            }
        }
        tokio::time::sleep(Duration::from_millis(500)).await;
    }
    Err(format!("{name:?} did not show up over Wi-Fi Direct"))
}
