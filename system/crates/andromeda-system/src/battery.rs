//! `/org/andromeda/System1/Battery/<id>`: one object per UPower device except line power, plus the composite
//! `DisplayDevice`. Stage 1 backend: UPower. Also writes `Power.OnBattery`.

use std::collections::HashMap;

use anyhow::{Result, anyhow};
use futures_util::StreamExt;
use tokio::task::JoinHandle;
use zbus::message::Header;
use zbus::object_server::InterfaceRef;
use zbus::{Connection, OwnedObjectPath, interface};

use crate::bus::{self, Props, Remote};
use crate::holds::KnobValue;
use crate::policy::{CHARGE_LIMIT, Shared};
use crate::{knobs, manager, plugs, polkit, power};

pub const AREA: &str = "battery";
pub const UPOWER: &str = "org.freedesktop.UPower";
const UPOWER_PATH: &str = "/org/freedesktop/UPower";
const DEVICE_IFACE: &str = "org.freedesktop.UPower.Device";
const PATH_PREFIX: &str = "/org/andromeda/System1/Battery/";
const LINE_POWER: u32 = 1;

#[zbus::proxy(
    interface = "org.freedesktop.UPower",
    default_service = "org.freedesktop.UPower",
    default_path = "/org/freedesktop/UPower"
)]
pub trait UPower {
    fn enumerate_devices(&self) -> zbus::Result<Vec<OwnedObjectPath>>;
    fn enumerate_kbd_backlights(&self) -> zbus::Result<Vec<OwnedObjectPath>>;
    fn get_display_device(&self) -> zbus::Result<OwnedObjectPath>;
    #[zbus(signal)]
    fn device_added(&self, device: OwnedObjectPath) -> zbus::Result<()>;
    #[zbus(signal)]
    fn device_removed(&self, device: OwnedObjectPath) -> zbus::Result<()>;
}

#[zbus::proxy(interface = "org.freedesktop.UPower.Device", default_service = "org.freedesktop.UPower")]
trait Device {
    fn get_history(&self, kind: &str, timespan: u32, resolution: u32) -> zbus::Result<Vec<(u32, f64, u32)>>;
    fn enable_charge_threshold(&self, enabled: bool) -> zbus::Result<()>;
    #[zbus(property)]
    fn charge_threshold_supported(&self) -> zbus::Result<bool>;
    #[zbus(property)]
    fn charge_threshold_enabled(&self) -> zbus::Result<bool>;
}

async fn devices(conn: &Connection) -> Result<Vec<DeviceProxy<'static>>> {
    let mut list = Vec::new();
    for path in UPowerProxy::new(conn).await?.enumerate_devices().await? {
        let device = DeviceProxy::builder(conn).path(path).cache_properties(zbus::proxy::CacheProperties::No);
        list.push(device.build().await?);
    }
    Ok(list)
}

/// The `battery.charge_limit` backend: turns the limit on or off on each battery that has one.
pub async fn set_charge_limit(conn: &Connection, on: bool) -> Result<()> {
    for device in devices(conn).await? {
        if device.charge_threshold_supported().await.unwrap_or_default()
            && device.charge_threshold_enabled().await.ok() != Some(on)
        {
            device.enable_charge_threshold(on).await?;
        }
    }
    Ok(())
}

/// Whether a battery has its limit on: the knob's state before the user's first choice.
pub async fn charge_limit_enabled(conn: &Connection) -> Result<bool> {
    for device in devices(conn).await? {
        if device.charge_threshold_supported().await.unwrap_or_default()
            && device.charge_threshold_enabled().await.unwrap_or_default()
        {
            return Ok(true);
        }
    }
    Ok(false)
}

const KINDS: [&str; 29] = [
    "unknown",
    "line-power",
    "battery",
    "ups",
    "monitor",
    "mouse",
    "keyboard",
    "pda",
    "phone",
    "media-player",
    "tablet",
    "computer",
    "gaming-input",
    "pen",
    "touchpad",
    "modem",
    "network",
    "headset",
    "speakers",
    "headphones",
    "video",
    "other-audio",
    "remote-control",
    "printer",
    "scanner",
    "camera",
    "wearable",
    "toy",
    "bluetooth-generic",
];
const STATES: [&str; 7] =
    ["unknown", "charging", "discharging", "empty", "fully-charged", "pending-charge", "pending-discharge"];
const WARNINGS: [&str; 6] = ["unknown", "none", "discharging", "low", "critical", "action"];

fn name_of(table: &[&'static str], index: Option<u32>) -> String {
    index.and_then(|i| table.get(i as usize)).copied().unwrap_or("unknown").to_owned()
}

#[derive(Default, Clone, PartialEq)]
pub struct BatteryData {
    pub kind: String,
    pub native_path: String,
    pub vendor: String,
    pub model: String,
    pub serial: String,
    pub power_supply: bool,
    pub present: bool,
    pub percent: f64,
    pub state: String,
    pub rate_w: f64,
    pub energy_wh: f64,
    pub energy_full_wh: f64,
    pub energy_full_design_wh: f64,
    pub time_to_empty_s: i64,
    pub time_to_full_s: i64,
    pub warning: String,
    pub health_percent: f64,
    pub cycles: i32,
    /// supported, enabled, start %, end %, settings bitmask (1 start, 2 end, 4 firmware); `u32::MAX` = not set.
    pub charge_limit: (bool, bool, u32, u32, u32),
}

impl BatteryData {
    fn from_props(p: &Props) -> Self {
        Self {
            kind: name_of(&KINDS, bus::get(p, "Type")),
            native_path: bus::get(p, "NativePath").unwrap_or_default(),
            vendor: bus::get(p, "Vendor").unwrap_or_default(),
            model: bus::get(p, "Model").unwrap_or_default(),
            serial: bus::get(p, "Serial").unwrap_or_default(),
            power_supply: bus::get(p, "PowerSupply").unwrap_or_default(),
            present: bus::get(p, "IsPresent").unwrap_or_default(),
            percent: bus::get(p, "Percentage").unwrap_or_default(),
            state: name_of(&STATES, bus::get(p, "State")),
            rate_w: bus::get(p, "EnergyRate").unwrap_or_default(),
            energy_wh: bus::get(p, "Energy").unwrap_or_default(),
            energy_full_wh: bus::get(p, "EnergyFull").unwrap_or_default(),
            energy_full_design_wh: bus::get(p, "EnergyFullDesign").unwrap_or_default(),
            time_to_empty_s: bus::get(p, "TimeToEmpty").unwrap_or_default(),
            time_to_full_s: bus::get(p, "TimeToFull").unwrap_or_default(),
            warning: name_of(&WARNINGS, bus::get(p, "WarningLevel")),
            health_percent: bus::get(p, "Capacity").unwrap_or_default(),
            cycles: bus::get(p, "ChargeCycles").unwrap_or(-1),
            charge_limit: (
                bus::get(p, "ChargeThresholdSupported").unwrap_or_default(),
                bus::get(p, "ChargeThresholdEnabled").unwrap_or_default(),
                bus::get(p, "ChargeStartThreshold").unwrap_or(u32::MAX),
                bus::get(p, "ChargeEndThreshold").unwrap_or(u32::MAX),
                bus::get(p, "ChargeThresholdSettingsSupported").unwrap_or_default(),
            ),
        }
    }
}

pub struct Battery {
    upower_path: OwnedObjectPath,
    data: BatteryData,
    policy: Shared,
}

#[interface(name = "org.andromeda.System1.Battery")]
impl Battery {
    /// battery, ups, mouse, keyboard, phone, headset, ...: UPower's device types in kebab case.
    #[zbus(property)]
    fn kind(&self) -> String {
        self.data.kind.clone()
    }
    #[zbus(property)]
    fn native_path(&self) -> String {
        self.data.native_path.clone()
    }
    #[zbus(property)]
    fn vendor(&self) -> String {
        self.data.vendor.clone()
    }
    #[zbus(property)]
    fn model(&self) -> String {
        self.data.model.clone()
    }
    #[zbus(property)]
    fn serial(&self) -> String {
        self.data.serial.clone()
    }
    /// true for a battery that powers this machine (not a peripheral).
    #[zbus(property)]
    fn power_supply(&self) -> bool {
        self.data.power_supply
    }
    #[zbus(property)]
    fn present(&self) -> bool {
        self.data.present
    }
    #[zbus(property)]
    fn percent(&self) -> f64 {
        self.data.percent
    }
    /// unknown, charging, discharging, empty, fully-charged, pending-charge, pending-discharge.
    #[zbus(property)]
    fn state(&self) -> String {
        self.data.state.clone()
    }
    #[zbus(property, name = "RateW")]
    fn rate_w(&self) -> f64 {
        self.data.rate_w
    }
    #[zbus(property, name = "EnergyWh")]
    fn energy_wh(&self) -> f64 {
        self.data.energy_wh
    }
    #[zbus(property, name = "EnergyFullWh")]
    fn energy_full_wh(&self) -> f64 {
        self.data.energy_full_wh
    }
    #[zbus(property, name = "EnergyFullDesignWh")]
    fn energy_full_design_wh(&self) -> f64 {
        self.data.energy_full_design_wh
    }
    /// Seconds; 0 = unknown.
    #[zbus(property, name = "TimeToEmptyS")]
    fn time_to_empty_s(&self) -> i64 {
        self.data.time_to_empty_s
    }
    /// Seconds; 0 = unknown.
    #[zbus(property, name = "TimeToFullS")]
    fn time_to_full_s(&self) -> i64 {
        self.data.time_to_full_s
    }
    /// unknown, none, discharging (UPS only), low, critical, action.
    #[zbus(property)]
    fn warning(&self) -> String {
        self.data.warning.clone()
    }
    #[zbus(property)]
    fn health_percent(&self) -> f64 {
        self.data.health_percent
    }
    /// -1 = not reported.
    #[zbus(property)]
    fn cycles(&self) -> i32 {
        self.data.cycles
    }
    /// (supported, enabled, start %, end %, settings bitmask 1 start | 2 end | 4 firmware); 4294967295 = not set.
    #[zbus(property)]
    fn charge_limit(&self) -> (bool, bool, u32, u32, u32) {
        self.data.charge_limit
    }

    /// The user's charge-limit choice: on or off, as `Knobs.Set` of `battery.charge_limit`, so it applies to each
    /// battery with a limit and ends the automatic holds on it. Start and end take 4294967295 (keep); values come
    /// from UPower (hwdb or sysfs) until the system part writes them (docs/system-health.md §5).
    async fn set_charge_limit(
        &self,
        enabled: bool,
        start: u32,
        end: u32,
        #[zbus(header)] header: Header<'_>,
        #[zbus(connection)] conn: &Connection,
    ) -> zbus::fdo::Result<()> {
        polkit::check(conn, &header, "org.andromeda.system.charge-limit").await?;
        if start != u32::MAX || end != u32::MAX {
            return Err(zbus::fdo::Error::NotSupported("charge-limit values are not settable yet".into()));
        }
        if !self.data.charge_limit.0 {
            return Err(zbus::fdo::Error::NotSupported("this battery has no charge limit".into()));
        }
        knobs::set_user(conn, &self.policy, CHARGE_LIMIT, KnobValue::Bool(enabled)).await
    }

    /// UPower's history: kind `rate` or `charge`, timespan in seconds, resolution in points; returns
    /// (unix time, value, state index as in `State`).
    async fn get_history(
        &self,
        kind: &str,
        timespan: u32,
        resolution: u32,
        #[zbus(connection)] conn: &Connection,
    ) -> zbus::fdo::Result<Vec<(u32, f64, u32)>> {
        let device = DeviceProxy::builder(conn).path(self.upower_path.clone()).build().await?;
        Ok(device.get_history(kind, timespan, resolution).await?)
    }
}

fn object_path(upower_path: &str) -> String {
    let base = upower_path.rsplit('/').next().unwrap_or_default();
    let id: String = base.chars().map(|c| if c.is_ascii_alphanumeric() { c } else { '_' }).collect();
    format!("{PATH_PREFIX}{}", if id.is_empty() { "_" } else { &id })
}

#[allow(clippy::float_cmp)] // exact change detection: any new value is published
async fn update(iface: &InterfaceRef<Battery>, data: BatteryData) -> zbus::Result<()> {
    let old = std::mem::replace(&mut iface.get_mut().await.data, data.clone());
    let battery = iface.get().await;
    let e = iface.signal_emitter();
    macro_rules! emit {
        ($($field:ident => $signal:ident),* $(,)?) => {
            $(if old.$field != data.$field { battery.$signal(e).await?; })*
        };
    }
    emit!(
        kind => kind_changed, native_path => native_path_changed, vendor => vendor_changed, model => model_changed,
        serial => serial_changed, power_supply => power_supply_changed, present => present_changed,
        percent => percent_changed, state => state_changed, rate_w => rate_w_changed, energy_wh => energy_wh_changed,
        energy_full_wh => energy_full_wh_changed, energy_full_design_wh => energy_full_design_wh_changed,
        time_to_empty_s => time_to_empty_s_changed, time_to_full_s => time_to_full_s_changed,
        warning => warning_changed, health_percent => health_percent_changed, cycles => cycles_changed,
        charge_limit => charge_limit_changed,
    );
    Ok(())
}

/// Follows one UPower device until its task is aborted. Line-power devices get no object.
async fn follow_device(conn: Connection, upower_path: OwnedObjectPath, policy: Shared) -> Result<()> {
    let mut remote = Remote::new(&conn, UPOWER, upower_path.to_string(), DEVICE_IFACE).await?;
    if bus::get::<u32>(&remote.props, "Type") == Some(LINE_POWER) {
        return Ok(());
    }
    let path = object_path(upower_path.as_str());
    let battery = Battery { upower_path, data: BatteryData::from_props(&remote.props), policy };
    // The composite DisplayDevice has no charge limit of its own.
    let system = battery.data.power_supply
        && battery.data.kind == "battery"
        && !battery.upower_path.as_str().ends_with("/DisplayDevice");
    let supported = battery.data.charge_limit.0;
    conn.object_server().at(path.as_str(), battery).await?;
    if system {
        power::update(&conn, |d| d.charge_limit_supported = supported).await?;
    }
    let iface: InterfaceRef<Battery> = conn.object_server().interface(path.as_str()).await?;
    while let Some(change) = remote.next().await {
        change?;
        update(&iface, BatteryData::from_props(&remote.props)).await?;
    }
    Ok(())
}

struct Devices {
    conn: Connection,
    policy: Shared,
    tasks: HashMap<OwnedObjectPath, JoinHandle<()>>,
}

impl Devices {
    fn add(&mut self, path: OwnedObjectPath) {
        if self.tasks.contains_key(&path) || crate::kbd_backlight::is_kbd_path(path.as_str()) {
            return;
        }
        let (conn, policy) = (self.conn.clone(), self.policy.clone());
        let key = path.clone();
        let task = tokio::spawn(async move {
            if let Err(e) = follow_device(conn.clone(), path.clone(), policy).await {
                log::warn!("battery {path}: {e:#}");
            }
            let _ = conn.object_server().remove::<Battery, _>(object_path(path.as_str())).await;
        });
        self.tasks.insert(key, task);
    }

    async fn remove(&mut self, path: &OwnedObjectPath) {
        if let Some(task) = self.tasks.remove(path) {
            task.abort();
        }
        let _ = self.conn.object_server().remove::<Battery, _>(object_path(path.as_str())).await;
    }

    async fn clear(&mut self) {
        let paths: Vec<_> = self.tasks.keys().cloned().collect();
        for path in &paths {
            self.remove(path).await;
        }
    }
}

/// Publishes the power source and records it in the plug history.
async fn on_battery(conn: &Connection, on_battery: bool) -> zbus::Result<()> {
    let now = crate::policy::now_ms() / 1000;
    plugs::record(!on_battery, now);
    let learned = plugs::learned(now).and_then(|m| i32::try_from(m).ok()).unwrap_or(-1);
    power::update(conn, |d| {
        d.on_battery = on_battery;
        d.learned_unplug_min = learned;
    })
    .await
}

/// Follows UPower for the life of the daemon.
pub async fn run(conn: Connection, policy: Shared) {
    let devices = &tokio::sync::Mutex::new(Devices { conn: conn.clone(), policy, tasks: HashMap::new() });
    let conn = &conn;
    bus::supervise(
        conn,
        UPOWER,
        AREA,
        move || async move { follow(conn, &mut *devices.lock().await).await },
        move || async move {
            devices.lock().await.clear().await;
            power::update(conn, |d| d.on_battery = false).await
        },
    )
    .await;
}

async fn follow(conn: &Connection, devices: &mut Devices) -> Result<()> {
    let gone = bus::vanished(conn, UPOWER);
    let upower = UPowerProxy::new(conn).await?;
    let mut added = upower.receive_device_added().await?;
    let mut removed = upower.receive_device_removed().await?;
    let mut manager = Remote::new(conn, UPOWER, UPOWER_PATH.to_owned(), UPOWER).await?;
    on_battery(conn, bus::get(&manager.props, "OnBattery").unwrap_or_default()).await?;
    devices.add(upower.get_display_device().await?);
    for path in upower.enumerate_devices().await? {
        devices.add(path);
    }
    manager::set_area(conn, AREA, true, "").await?;
    tokio::pin!(gone);
    loop {
        tokio::select! {
            result = &mut gone => return result,
            change = manager.next() => match change {
                Some(Ok(())) => on_battery(conn, bus::get(&manager.props, "OnBattery").unwrap_or_default()).await?,
                Some(Err(e)) => return Err(e),
                None => return Err(anyhow!("signal stream ended")),
            },
            Some(signal) = added.next() => devices.add(signal.args()?.device().clone()),
            Some(signal) = removed.next() => devices.remove(signal.args()?.device()).await,
        }
    }
}
