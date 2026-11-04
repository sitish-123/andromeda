//! `/org/andromeda/System1/Power`: power source and profile. Stage 1 backend: power-profiles-daemon (profile);
//! `OnBattery` is written by the UPower backend (battery.rs).

use std::collections::HashMap;

use anyhow::{Result, anyhow};
use zbus::object_server::InterfaceRef;
use zbus::{Connection, OwnedVariant, interface};

use crate::bus::{self, Remote};
use crate::manager;

pub const PATH: &str = "/org/andromeda/System1/Power";
pub const AREA: &str = "power";
pub const PPD: &str = "org.freedesktop.UPower.PowerProfiles";
const PPD_PATH: &str = "/org/freedesktop/UPower/PowerProfiles";

/// `intel-rapl:0` is the package domain (also on AMD); `intel-rapl-mmio:0` repeats it, so it is never added.
const RAPL: &str = "/sys/class/powercap/intel-rapl:0";

#[derive(Clone, PartialEq)]
pub struct PowerData {
    pub on_battery: bool,
    pub profile: String,
    pub profile_choices: Vec<String>,
    pub performance_degraded: String,
    pub package_power_w: f64,
    /// Minutes after midnight UTC; -1 until 14 days of plug history.
    pub learned_unplug_min: i32,
    pub charge_limit_supported: bool,
}

impl Default for PowerData {
    fn default() -> Self {
        Self {
            on_battery: false,
            profile: String::new(),
            profile_choices: Vec::new(),
            performance_degraded: String::new(),
            package_power_w: f64::NAN,
            learned_unplug_min: -1,
            charge_limit_supported: false,
        }
    }
}

#[derive(Default)]
pub struct Power {
    data: PowerData,
    /// RAPL package counter at the last `PackageEnergyJ` call, and the joules counted since the daemon started.
    energy: std::sync::Mutex<(Option<u64>, f64)>,
}

#[interface(name = "org.andromeda.System1.Power")]
impl Power {
    #[zbus(property)]
    fn on_battery(&self) -> bool {
        self.data.on_battery
    }

    /// power-saver, balanced or performance; empty while no profile backend runs.
    #[zbus(property)]
    fn profile(&self) -> String {
        self.data.profile.clone()
    }

    #[zbus(property)]
    fn profile_choices(&self) -> Vec<String> {
        self.data.profile_choices.clone()
    }

    /// Empty, `lap-detected` or `high-operating-temperature` (power-profiles-daemon's reasons).
    #[zbus(property)]
    fn performance_degraded(&self) -> String {
        self.data.performance_degraded.clone()
    }

    /// CPU package power in W from RAPL, sampled each second while the `power` area is watched; NaN otherwise or
    /// where RAPL is missing.
    #[zbus(property, name = "PackagePowerW")]
    fn package_power_w(&self) -> f64 {
        self.data.package_power_w
    }

    /// CPU package energy in J since the daemon started, from RAPL; NaN where RAPL is missing. Read on demand, no
    /// timer: the hardware counter wraps after some hours at full load, so a caller reads it at least that often.
    fn package_energy_j(&self) -> f64 {
        let (Some(raw), Some(range)) =
            (read_u64(&format!("{RAPL}/energy_uj")), read_u64(&format!("{RAPL}/max_energy_range_uj")))
        else {
            return f64::NAN;
        };
        let Ok(mut energy) = self.energy.lock() else { return f64::NAN };
        if let Some(last) = energy.0 {
            energy.1 += rapl_joules(last, raw, range);
        }
        energy.0 = Some(raw);
        energy.1
    }

    /// The usual time the charger comes out after an overnight charge, in minutes after midnight UTC, learned from
    /// 14 days of plug history; -1 before that.
    #[zbus(property)]
    fn learned_unplug_min(&self) -> i32 {
        self.data.learned_unplug_min
    }

    /// Whether the machine's battery has a charge limit (UPower charge thresholds).
    #[zbus(property)]
    fn charge_limit_supported(&self) -> bool {
        self.data.charge_limit_supported
    }
}

/// Applies `change` and emits `PropertiesChanged` for the fields that differ.
pub async fn update(conn: &Connection, change: impl FnOnce(&mut PowerData)) -> zbus::Result<()> {
    let iface: InterfaceRef<Power> = conn.object_server().interface(PATH).await?;
    let (old, new) = {
        let mut power = iface.get_mut().await;
        let old = power.data.clone();
        change(&mut power.data);
        (old, power.data.clone())
    };
    let power = iface.get().await;
    let emitter = iface.signal_emitter();
    if old.on_battery != new.on_battery {
        power.on_battery_changed(emitter).await?;
    }
    if old.profile != new.profile {
        power.profile_changed(emitter).await?;
    }
    if old.profile_choices != new.profile_choices {
        power.profile_choices_changed(emitter).await?;
    }
    if old.performance_degraded != new.performance_degraded {
        power.performance_degraded_changed(emitter).await?;
    }
    if old.package_power_w.to_bits() != new.package_power_w.to_bits() {
        power.package_power_w_changed(emitter).await?;
    }
    if old.learned_unplug_min != new.learned_unplug_min {
        power.learned_unplug_min_changed(emitter).await?;
    }
    if old.charge_limit_supported != new.charge_limit_supported {
        power.charge_limit_supported_changed(emitter).await?;
    }
    Ok(())
}

fn profile_fields(props: &bus::Props, data: &mut PowerData) {
    data.profile = bus::get(props, "ActiveProfile").unwrap_or_default();
    data.performance_degraded = bus::get(props, "PerformanceDegraded").unwrap_or_default();
    data.profile_choices = bus::get::<Vec<HashMap<String, OwnedVariant>>>(props, "Profiles")
        .unwrap_or_default()
        .iter()
        .filter_map(|p| bus::get::<String>(p, "Profile"))
        .collect();
}

/// Follows power-profiles-daemon for the life of the daemon.
pub async fn run(conn: Connection) {
    let clear = |d: &mut PowerData| {
        d.profile.clear();
        d.profile_choices.clear();
        d.performance_degraded.clear();
    };
    let conn = &conn;
    bus::supervise(conn, PPD, AREA, move || follow(conn), move || update(conn, clear)).await;
}

async fn follow(conn: &Connection) -> Result<()> {
    let gone = bus::vanished(conn, PPD);
    let mut remote = Remote::new(conn, PPD, PPD_PATH.to_owned(), PPD).await?;
    update(conn, |d| profile_fields(&remote.props, d)).await?;
    manager::set_area(conn, AREA, true, "").await?;
    tokio::pin!(gone);
    loop {
        tokio::select! {
            result = &mut gone => return result,
            change = remote.next() => match change {
                Some(Ok(())) => update(conn, |d| profile_fields(&remote.props, d)).await?,
                Some(Err(e)) => return Err(e),
                None => return Err(anyhow!("signal stream ended")),
            },
        }
    }
}

/// The user's profile choice. power-profiles-daemon drops every hold when `ActiveProfile` is set; step 2 re-places
/// the automatic holds after this call.
pub async fn set_profile(conn: &Connection, profile: &str) -> Result<()> {
    let choices = conn.object_server().interface::<_, Power>(PATH).await?.get().await.data.profile_choices.clone();
    if !choices.iter().any(|c| c == profile) {
        return Err(anyhow!("unknown profile {profile:?}; choices: {choices:?}"));
    }
    let proxy = zbus::fdo::PropertiesProxy::builder(conn).destination(PPD).path(PPD_PATH).build().await?;
    proxy
        .set(zbus::names::InterfaceName::from_static_str(PPD)?, "ActiveProfile", &zbus::Variant::from(profile))
        .await?;
    Ok(())
}

/// Energy between two `energy_uj` readings; the counter wraps at `max_energy_range_uj`.
#[allow(clippy::cast_precision_loss)] // µJ deltas stay far below 2^52
pub fn rapl_joules(before_uj: u64, after_uj: u64, range_uj: u64) -> f64 {
    let delta = if after_uj >= before_uj { after_uj - before_uj } else { range_uj - before_uj + after_uj };
    delta as f64 / 1e6
}

/// Average power between two `energy_uj` readings.
pub fn rapl_watts(before_uj: u64, after_uj: u64, range_uj: u64, seconds: f64) -> f64 {
    rapl_joules(before_uj, after_uj, range_uj) / seconds
}

fn read_u64(path: &str) -> Option<u64> {
    std::fs::read_to_string(path).ok()?.trim().parse().ok()
}

/// Samples RAPL each second while the `power` area is watched; no timer runs otherwise.
pub async fn sample_rapl(conn: Connection) {
    let mut watched = manager::watched();
    let range = read_u64(&format!("{RAPL}/max_energy_range_uj"));
    loop {
        if !watched.borrow_and_update().contains(AREA) {
            let _ = update(&conn, |d| d.package_power_w = f64::NAN).await;
            if watched.changed().await.is_err() {
                return;
            }
            continue;
        }
        let (Some(range), Some(mut before)) = (range, read_u64(&format!("{RAPL}/energy_uj"))) else {
            log::info!("power: no readable RAPL package counter; PackagePowerW stays NaN");
            return;
        };
        let mut at = tokio::time::Instant::now();
        let mut tick = tokio::time::interval(std::time::Duration::from_secs(1));
        tick.tick().await;
        while watched.borrow().contains(AREA) {
            tokio::select! {
                now = tick.tick() => {
                    let Some(after) = read_u64(&format!("{RAPL}/energy_uj")) else { break };
                    let watts = rapl_watts(before, after, range, (now - at).as_secs_f64());
                    (before, at) = (after, now);
                    let _ = update(&conn, |d| d.package_power_w = watts).await;
                }
                changed = watched.changed() => if changed.is_err() { return },
            }
        }
    }
}

/// Places a power-profiles-daemon hold; it ends with [`release_profile`] or when this daemon leaves the bus.
pub async fn hold_profile(conn: &Connection, profile: &str, reason: &str) -> Result<u32> {
    let reply =
        conn.call_method(Some(PPD), PPD_PATH, Some(PPD), "HoldProfile", &(profile, reason, crate::NAME)).await?;
    Ok(reply.body().deserialize()?)
}

pub async fn release_profile(conn: &Connection, cookie: u32) -> Result<()> {
    conn.call_method(Some(PPD), PPD_PATH, Some(PPD), "ReleaseProfile", &(cookie,)).await?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::rapl_watts;

    #[test]
    fn rapl_wraps_at_the_range() {
        assert!((rapl_watts(1_000_000, 3_000_000, 10_000_000, 1.0) - 2.0).abs() < 1e-9);
        assert!((rapl_watts(9_000_000, 1_000_000, 10_000_000, 2.0) - 1.0).abs() < 1e-9);
    }
}
