//! Firmware updates from fwupd (`org.freedesktop.fwupd` on the system bus); installs go through fwupd's own polkit
//! actions, not ours.

use std::collections::HashMap;

use anyhow::Result;
use zbus::Connection;
use zbus::OwnedVariant as OwnedValue;

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Update {
    pub device: String,
    pub current: String,
    pub version: String,
    pub size: u64,
}

type Dict = HashMap<String, OwnedValue>;

#[zbus::proxy(interface = "org.freedesktop.fwupd", default_service = "org.freedesktop.fwupd", default_path = "/")]
trait Fwupd {
    fn get_devices(&self) -> zbus::Result<Vec<Dict>>;
    fn get_upgrades(&self, device_id: &str) -> zbus::Result<Vec<Dict>>;
}

fn text(dict: &Dict, key: &str) -> String {
    dict.get(key).and_then(|v| String::try_from(v.clone()).ok()).unwrap_or_default()
}

/// Devices with an upgrade; fwupd missing means none. A device fwupd cannot update answers with an error: skipped.
pub async fn check(system: &Connection) -> Result<Vec<Update>> {
    let Ok(fwupd) = FwupdProxy::new(system).await else { return Ok(Vec::new()) };
    let devices = match fwupd.get_devices().await {
        Ok(devices) => devices,
        // No fwupd, or no devices it can handle (VMs). zbus returns org.freedesktop.DBus.Error.* replies as FDO.
        Err(zbus::Error::FDO(_)) => return Ok(Vec::new()),
        Err(zbus::Error::MethodError(name, _, _))
            if name.as_str().starts_with("org.freedesktop.DBus.Error")
                || matches!(name.as_str(), "org.freedesktop.fwupd.NothingToDo" | "org.freedesktop.fwupd.NotSupported") =>
        {
            return Ok(Vec::new());
        }
        Err(e) => return Err(e.into()),
    };
    let mut updates = Vec::new();
    for device in &devices {
        let id = text(device, "DeviceId");
        let Ok(releases) = fwupd.get_upgrades(&id).await else { continue };
        if let Some(release) = releases.first() {
            updates.push(Update {
                device: text(device, "Name"),
                current: text(device, "Version"),
                version: text(release, "Version"),
                size: release.get("Size").and_then(|v| u64::try_from(v.clone()).ok()).unwrap_or(0),
            });
        }
    }
    Ok(updates)
}
