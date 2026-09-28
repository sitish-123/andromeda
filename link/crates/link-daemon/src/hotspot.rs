//! Joining a phone's hotspot through `NetworkManager`, for transfers too large for Bluetooth. The profile is volatile:
//! `NetworkManager` deletes it once it deactivates, and autoconnects the desktop's usual network again by itself.

use std::collections::HashMap;
use std::net::IpAddr;
use std::time::Duration;

use zbus::zvariant::{ObjectPath, OwnedObjectPath, OwnedValue, Value};

pub const NM: &str = "org.freedesktop.NetworkManager";
pub const NM_PATH: &str = "/org/freedesktop/NetworkManager";
/// Joining includes the Wi-Fi scan and DHCP.
const JOIN_TIMEOUT: Duration = Duration::from_secs(30);
const ACTIVATED: u32 = 2;
const DEACTIVATED: u32 = 4;

/// An active hotspot connection and the address the desktop got on it.
pub struct Joined {
    pub active: OwnedObjectPath,
    pub address: IpAddr,
}

pub async fn join(ssid: &str, passphrase: &str) -> Result<Joined, String> {
    let bus = zbus::Connection::system().await.map_err(text)?;
    let manager = zbus::Proxy::new(&bus, NM, NM_PATH, NM).await.map_err(text)?;
    let user = std::env::var("USER").unwrap_or_default();
    let settings: HashMap<&str, HashMap<&str, Value<'_>>> = HashMap::from([
        (
            "connection",
            HashMap::from([
                ("id", Value::from(format!("Umbriel Link: {ssid}"))),
                ("type", Value::from("802-11-wireless")),
                ("autoconnect", Value::from(false)),
                ("permissions", Value::from(vec![format!("user:{user}")])),
            ]),
        ),
        (
            "802-11-wireless",
            HashMap::from([("ssid", Value::from(ssid.as_bytes().to_vec())), ("mode", Value::from("infrastructure"))]),
        ),
        (
            "802-11-wireless-security",
            HashMap::from([("key-mgmt", Value::from("wpa-psk")), ("psk", Value::from(passphrase))]),
        ),
        ("ipv4", HashMap::from([("method", Value::from("auto"))])),
        ("ipv6", HashMap::from([("method", Value::from("ignore"))])),
    ]);
    let root = ObjectPath::try_from("/").map_err(text)?;
    let options: HashMap<&str, Value<'_>> = HashMap::from([("persist", Value::from("volatile"))]);
    let (_, active, _): (OwnedObjectPath, OwnedObjectPath, HashMap<String, OwnedValue>) =
        manager.call("AddAndActivateConnection2", &(settings, &root, &root, options)).await.map_err(text)?;
    activated(&bus, active).await
}

/// Waits for `active` to come up and reads the address the desktop got on it; shared with Wi-Fi Direct.
pub async fn activated(bus: &zbus::Connection, active: OwnedObjectPath) -> Result<Joined, String> {
    let connection = zbus::Proxy::new(bus, NM, active.as_str(), "org.freedesktop.NetworkManager.Connection.Active")
        .await
        .map_err(text)?;
    let activated = async {
        loop {
            match connection.get_property::<u32>("State").await.map_err(text)? {
                ACTIVATED => return Ok(()),
                DEACTIVATED => return Err("NetworkManager could not join it".to_owned()),
                _ => tokio::time::sleep(Duration::from_millis(250)).await,
            }
        }
    };
    tokio::time::timeout(JOIN_TIMEOUT, activated).await.map_err(|_| "joining timed out".to_owned())??;
    let config: OwnedObjectPath = connection.get_property("Ip4Config").await.map_err(text)?;
    let ip4 =
        zbus::Proxy::new(bus, NM, config.as_str(), "org.freedesktop.NetworkManager.IP4Config").await.map_err(text)?;
    let addresses: Vec<HashMap<String, OwnedValue>> = ip4.get_property("AddressData").await.map_err(text)?;
    let address = addresses
        .iter()
        .find_map(|entry| String::try_from(entry.get("address")?.clone()).ok()?.parse().ok())
        .ok_or_else(|| "no address on the hotspot".to_owned())?;
    Ok(Joined { active, address })
}

/// Leaves the hotspot; `NetworkManager` then deletes the profile and returns to the usual network.
pub async fn leave(active: &OwnedObjectPath) {
    let left = async {
        let bus = zbus::Connection::system().await?;
        let manager = zbus::Proxy::new(&bus, NM, NM_PATH, NM).await?;
        manager.call::<_, _, ()>("DeactivateConnection", &(active,)).await
    };
    if let Err(error) = left.await {
        log::info!("leaving the hotspot: {error}");
    }
}

pub fn text(error: impl std::fmt::Display) -> String {
    error.to_string()
}
