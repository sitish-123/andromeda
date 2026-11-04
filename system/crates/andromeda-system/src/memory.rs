//! `/org/andromeda/System1/Memory`: pressure level from PSI triggers (no timer while the level is normal and the area
//! is not watched), plus avg10 and available memory.

use std::os::fd::OwnedFd;
use std::time::Duration;

use anyhow::{Context, Result};
use rustix::event::{PollFd, PollFlags};
use rustix::fs::{Mode, OFlags};
use tokio::sync::mpsc;
use tokio::time::Instant;
use zbus::fdo::{RequestNameFlags, RequestNameReply};
use zbus::object_server::{InterfaceRef, SignalEmitter};
use zbus::{Connection, interface};

use crate::manager;

pub const PATH: &str = "/org/andromeda/System1/Memory";
pub const AREA: &str = "memory";
const PSI: &str = "/proc/pressure/memory";
/// Stall per 1 s window: medium 70 ms, critical 150 ms (docs/system-health.md §3.2).
const MEDIUM_US: u64 = 70_000;
const CRITICAL_US: u64 = 150_000;

#[derive(Clone, PartialEq)]
pub struct MemoryData {
    pub level: String,
    pub some_avg10: f64,
    pub full_avg10: f64,
    pub available_bytes: u64,
}

impl Default for MemoryData {
    fn default() -> Self {
        Self { level: "normal".to_owned(), some_avg10: 0.0, full_avg10: 0.0, available_bytes: 0 }
    }
}

#[derive(Default)]
pub struct Memory {
    data: MemoryData,
}

#[interface(name = "org.andromeda.System1.Memory")]
impl Memory {
    /// normal, medium (some stall >= 70 ms per 1 s) or critical (>= 150 ms per 1 s).
    #[zbus(property)]
    fn level(&self) -> String {
        self.data.level.clone()
    }
    /// PSI `some` avg10 in percent.
    #[zbus(property)]
    fn some_avg10(&self) -> f64 {
        self.data.some_avg10
    }
    /// PSI `full` avg10 in percent.
    #[zbus(property)]
    fn full_avg10(&self) -> f64 {
        self.data.full_avg10
    }
    /// `MemAvailable` from /proc/meminfo.
    #[zbus(property)]
    fn available_bytes(&self) -> u64 {
        self.data.available_bytes
    }
}

/// low-memory-monitor's bus name: `GLib`'s `GMemoryMonitor` (and through it the portal) listens to its owner.
pub const LMM_NAME: &str = "org.freedesktop.LowMemoryMonitor";
const LMM_PATH: &str = "/org/freedesktop/LowMemoryMonitor";
const LMM_ACTIVATABLE: &str = "/usr/share/dbus-1/system-services/org.freedesktop.LowMemoryMonitor.service";

pub struct LowMemoryMonitor;

#[interface(name = "org.freedesktop.LowMemoryMonitor")]
impl LowMemoryMonitor {
    /// `GLib` `GMemoryMonitorWarningLevel`: 50 low, 100 medium, 255 critical.
    #[zbus(signal)]
    async fn low_memory_warning(emitter: &SignalEmitter<'_>, state: u8) -> zbus::Result<()>;
}

/// Takes low-memory-monitor's name when that service is not installed (docs/system-health.md §9).
pub async fn serve_low_memory_monitor(conn: &Connection) -> zbus::Result<bool> {
    if std::path::Path::new(LMM_ACTIVATABLE).exists() {
        return Ok(false);
    }
    if conn.request_name_with_flags(LMM_NAME, RequestNameFlags::DO_NOT_QUEUE).await? != RequestNameReply::PrimaryOwner {
        return Ok(false);
    }
    conn.object_server().at(LMM_PATH, LowMemoryMonitor).await?;
    Ok(true)
}

fn warning_byte(level: &str) -> u8 {
    match level {
        "critical" => 255,
        "medium" => 100,
        _ => 0,
    }
}

#[allow(clippy::float_cmp)] // exact change detection: any new value is published
async fn update(conn: &Connection, data: MemoryData) -> zbus::Result<()> {
    let iface: InterfaceRef<Memory> = conn.object_server().interface(PATH).await?;
    let old = std::mem::replace(&mut iface.get_mut().await.data, data.clone());
    let memory = iface.get().await;
    let e = iface.signal_emitter();
    if old.level != data.level {
        memory.level_changed(e).await?;
        if warning_byte(&data.level) > warning_byte(&old.level)
            && let Ok(lmm) = conn.object_server().interface::<_, LowMemoryMonitor>(LMM_PATH).await
        {
            LowMemoryMonitor::low_memory_warning(lmm.signal_emitter(), warning_byte(&data.level)).await?;
        }
    }
    if old.some_avg10 != data.some_avg10 {
        memory.some_avg10_changed(e).await?;
    }
    if old.full_avg10 != data.full_avg10 {
        memory.full_avg10_changed(e).await?;
    }
    if old.available_bytes != data.available_bytes {
        memory.available_bytes_changed(e).await?;
    }
    Ok(())
}

/// (some avg10, full avg10) from the text of a /proc/pressure file.
pub fn parse_psi(text: &str) -> (f64, f64) {
    let avg10 = |kind: &str| {
        text.lines()
            .find(|l| l.starts_with(kind))
            .and_then(|l| l.split_ascii_whitespace().find_map(|f| f.strip_prefix("avg10=")))
            .and_then(|v| v.parse().ok())
            .unwrap_or(0.0)
    };
    (avg10("some "), avg10("full "))
}

fn available_bytes() -> u64 {
    std::fs::read_to_string("/proc/meminfo")
        .ok()
        .and_then(|t| {
            let line = t.lines().find(|l| l.starts_with("MemAvailable:"))?;
            line.split_ascii_whitespace().nth(1)?.parse::<u64>().ok()
        })
        .map_or(0, |kb| kb * 1024)
}

/// A PSI trigger fd that becomes ready (POLLPRI) at most once per window while the stall exceeds `stall_us`.
/// Root (`CAP_SYS_RESOURCE`) may use a 1 s window; others need a multiple of 2 s, so the stall scales with it.
fn trigger(stall_us: u64) -> Result<(OwnedFd, Duration)> {
    for window_s in [1, 2] {
        let fd = rustix::fs::open(PSI, OFlags::RDWR | OFlags::NONBLOCK | OFlags::CLOEXEC, Mode::empty())
            .with_context(|| format!("open {PSI}"))?;
        let spec = format!("some {} {}\0", stall_us * window_s, window_s * 1_000_000);
        if rustix::io::write(&fd, spec.as_bytes()).is_ok() {
            return Ok((fd, Duration::from_secs(window_s)));
        }
    }
    anyhow::bail!("{PSI}: the kernel refused the trigger")
}

#[derive(Clone, Copy)]
enum Fired {
    Medium,
    Critical,
}

/// A thread blocks in poll(2) on both triggers, so a normal level costs no wakeup; it ends on POLLERR.
fn watch_triggers(medium: OwnedFd, critical: OwnedFd) -> Result<mpsc::UnboundedReceiver<Fired>> {
    let (tx, rx) = mpsc::unbounded_channel();
    std::thread::Builder::new().name("psi-triggers".into()).spawn(move || {
        loop {
            let mut fds = [PollFd::new(&medium, PollFlags::PRI), PollFd::new(&critical, PollFlags::PRI)];
            if rustix::event::poll(&mut fds, None).is_err() {
                continue;
            }
            for (fd, kind) in fds.iter().zip([Fired::Medium, Fired::Critical]) {
                let events = fd.revents();
                if events.intersects(PollFlags::ERR | PollFlags::NVAL) {
                    log::warn!("memory: PSI trigger closed by the kernel");
                    return;
                }
                if events.contains(PollFlags::PRI) && tx.send(kind).is_err() {
                    return;
                }
            }
        }
    })?;
    Ok(rx)
}

pub async fn run(conn: Connection) {
    if let Err(e) = follow(&conn).await {
        log::warn!("memory: {e:#}");
        let _ = manager::set_area(&conn, AREA, false, &format!("{e:#}")).await;
    }
}

async fn follow(conn: &Connection) -> Result<()> {
    let (medium, window) = trigger(MEDIUM_US)?;
    let (critical, _) = trigger(CRITICAL_US)?;
    let mut fired = watch_triggers(medium, critical)?;
    // A trigger fires at most once per window; with no event for 1.5 windows the stall is below its threshold.
    let quiet = window + window / 2;
    let mut watched = manager::watched();
    let (mut last_medium, mut last_critical): (Option<Instant>, Option<Instant>) = (None, None);
    let mut tick = tokio::time::interval(Duration::from_secs(1).min(window));
    tick.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Delay);
    manager::set_area(conn, AREA, true, "").await?;
    loop {
        let now = Instant::now();
        let recent = |t: Option<Instant>| t.is_some_and(|t| now - t < quiet);
        let level = if recent(last_critical) {
            "critical"
        } else if recent(last_medium) {
            "medium"
        } else {
            "normal"
        };
        let (some_avg10, full_avg10) = parse_psi(&std::fs::read_to_string(PSI).unwrap_or_default());
        update(
            conn,
            MemoryData { level: level.to_owned(), some_avg10, full_avg10, available_bytes: available_bytes() },
        )
        .await?;
        let sampling = level != "normal" || watched.borrow_and_update().contains(AREA);
        tokio::select! {
            event = fired.recv() => match event {
                Some(Fired::Medium) => last_medium = Some(Instant::now()),
                Some(Fired::Critical) => last_critical = Some(Instant::now()),
                None => anyhow::bail!("PSI triggers ended"),
            },
            _ = tick.tick(), if sampling => {}
            r = watched.changed() => r?,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::parse_psi;

    #[test]
    fn psi_avg10_of_both_lines() {
        let text = "some avg10=1.25 avg60=0.40 avg300=0.10 total=123\nfull avg10=0.50 avg60=0.00 avg300=0.00 total=9\n";
        assert_eq!(parse_psi(text), (1.25, 0.5));
    }
}
