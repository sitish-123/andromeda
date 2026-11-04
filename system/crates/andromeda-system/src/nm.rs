//! The `network.wifi_power_save` actuator (docs/system-health.md §5): NetworkManager applies it to the live Wi-Fi
//! connection with `Device.Reapply`, as `nmcli device modify` does, so the stored profile stays unchanged.

use std::collections::HashMap;

use anyhow::Result;
use zbus::{Connection, OwnedObjectPath, OwnedVariant, Variant};

const NM: &str = "org.freedesktop.NetworkManager";
const DEVICE: &str = "org.freedesktop.NetworkManager.Device";
const DEVICE_TYPE_WIFI: u32 = 2;
/// `802-11-wireless.powersave`: 0 = the global default, 3 = enable (`NM_SETTING_WIRELESS_POWERSAVE_*`).
const POWERSAVE_DEFAULT: u32 = 0;
const POWERSAVE_ENABLE: u32 = 3;

type Settings = HashMap<String, HashMap<String, OwnedVariant>>;

/// Turns power save on for every active Wi-Fi connection, or back to the global default.
pub async fn set_wifi_power_save(system: &Connection, on: bool) -> Result<()> {
    let reply = system.call_method(Some(NM), "/org/freedesktop/NetworkManager", Some(NM), "GetDevices", &()).await?;
    let devices: Vec<OwnedObjectPath> = reply.body().deserialize()?;
    for path in devices {
        let props = zbus::fdo::PropertiesProxy::builder(system).destination(NM).path(path.clone()).build().await?;
        let kind = props.get(zbus::names::InterfaceName::from_static_str(DEVICE)?, "DeviceType").await?;
        if u32::try_from(&kind).ok() != Some(DEVICE_TYPE_WIFI) {
            continue;
        }
        let Ok(applied) = system.call_method(Some(NM), &path, Some(DEVICE), "GetAppliedConnection", &(0u32,)).await
        else {
            continue; // not connected
        };
        let (mut settings, version): (Settings, u64) = applied.body().deserialize()?;
        let value = OwnedVariant::try_from(Variant::from(if on { POWERSAVE_ENABLE } else { POWERSAVE_DEFAULT }))?;
        settings.entry("802-11-wireless".to_owned()).or_default().insert("powersave".to_owned(), value);
        system.call_method(Some(NM), &path, Some(DEVICE), "Reapply", &(settings, version, 0u32)).await?;
    }
    Ok(())
}
