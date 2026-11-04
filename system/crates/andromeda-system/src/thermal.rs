//! `/org/andromeda/System1/Thermal`: zones and their trip state, read from sysfs when the kernel's thermal netlink
//! `event` group reports a change (trip crossed, zone added), each second while watched, and at start.

use std::path::Path;
use std::sync::Arc;
use std::time::Duration;

use anyhow::{Context, Result, anyhow, bail};
use netlink_sys::{Socket, protocols::NETLINK_GENERIC};
use tokio::sync::Notify;
use zbus::object_server::InterfaceRef;
use zbus::{Connection, interface};

use crate::manager;

pub const PATH: &str = "/org/andromeda/System1/Thermal";
pub const AREA: &str = "thermal";
const ZONES: &str = "/sys/class/thermal";

/// (zone type, temperature °C, state: normal | passive | hot | critical, next trip above the temperature °C or NaN)
pub type Zone = (String, f64, String, f64);

#[derive(Default)]
pub struct Thermal {
    zones: Vec<Zone>,
}

#[interface(name = "org.andromeda.System1.Thermal")]
impl Thermal {
    /// (type, °C, state, next trip °C or NaN) per zone. Temperatures refresh on trip events and while watched.
    #[zbus(property)]
    fn zones(&self) -> Vec<Zone> {
        self.zones.clone()
    }
    /// A zone is past a passive (or hotter) trip, so the kernel or firmware slows the machine.
    #[zbus(property)]
    fn throttled(&self) -> bool {
        self.zones.iter().any(|z| z.2 != "normal")
    }
}

fn read_trimmed(path: &Path) -> Option<String> {
    std::fs::read_to_string(path).ok().map(|s| s.trim().to_owned())
}

fn millidegrees(path: &Path) -> Option<f64> {
    read_trimmed(path)?.parse::<i64>().ok().map(|m| {
        #[allow(clippy::cast_precision_loss)] // m°C values stay far below 2^52
        let c = m as f64 / 1000.0;
        c
    })
}

/// State and next trip of one zone from its trips (type, °C). Active trips (fans) do not count as throttling.
pub fn zone_state(temp: f64, trips: &[(String, f64)]) -> (String, f64) {
    let rank = |kind: &str| match kind {
        "passive" => 1,
        "hot" => 2,
        "critical" => 3,
        _ => 0,
    };
    let state = trips
        .iter()
        .filter(|(kind, t)| rank(kind) > 0 && temp >= *t)
        .max_by_key(|(kind, _)| rank(kind))
        .map_or("normal", |(kind, _)| kind.as_str());
    let next = trips.iter().map(|(_, t)| *t).filter(|t| *t > temp).fold(f64::NAN, f64::min);
    (state.to_owned(), next)
}

fn read_zones() -> Vec<Zone> {
    let Ok(entries) = std::fs::read_dir(ZONES) else { return Vec::new() };
    let mut zones: Vec<(String, Zone)> = entries
        .flatten()
        .filter(|e| e.file_name().to_string_lossy().starts_with("thermal_zone"))
        .filter_map(|e| {
            let dir = e.path();
            let kind = read_trimmed(&dir.join("type"))?;
            let temp = millidegrees(&dir.join("temp"))?;
            let trips: Vec<(String, f64)> = (0..32)
                .map_while(|i| {
                    let t = read_trimmed(&dir.join(format!("trip_point_{i}_type")))?;
                    Some((t, millidegrees(&dir.join(format!("trip_point_{i}_temp"))).unwrap_or(f64::NAN)))
                })
                .collect();
            let (state, next) = zone_state(temp, &trips);
            Some((e.file_name().to_string_lossy().into_owned(), (kind, temp, state, next)))
        })
        .collect();
    zones.sort_by(|a, b| a.0.cmp(&b.0));
    zones.into_iter().map(|(_, z)| z).collect()
}

const GENL_ID_CTRL: u16 = 0x10;
const NLM_F_REQUEST: u16 = 1;
const NLMSG_ERROR: u16 = 2;
const CTRL_CMD_GETFAMILY: u8 = 3;
const CTRL_ATTR_FAMILY_NAME: u16 = 2;
const CTRL_ATTR_MCAST_GROUPS: u16 = 7;
const CTRL_ATTR_MCAST_GRP_NAME: u16 = 1;
const CTRL_ATTR_MCAST_GRP_ID: u16 = 2;
const NLA_TYPE_MASK: u16 = 0x3fff;

fn getfamily_request(family: &str) -> Vec<u8> {
    let name_len = family.len() + 1;
    let attr_len = 4 + name_len;
    let total = 16 + 4 + attr_len.next_multiple_of(4);
    let mut msg = Vec::with_capacity(total);
    msg.extend_from_slice(&u32::try_from(total).unwrap_or(u32::MAX).to_ne_bytes());
    msg.extend_from_slice(&GENL_ID_CTRL.to_ne_bytes());
    msg.extend_from_slice(&NLM_F_REQUEST.to_ne_bytes());
    msg.extend_from_slice(&1u32.to_ne_bytes());
    msg.extend_from_slice(&0u32.to_ne_bytes());
    msg.extend_from_slice(&[CTRL_CMD_GETFAMILY, 1, 0, 0]);
    msg.extend_from_slice(&u16::try_from(attr_len).unwrap_or(u16::MAX).to_ne_bytes());
    msg.extend_from_slice(&CTRL_ATTR_FAMILY_NAME.to_ne_bytes());
    msg.extend_from_slice(family.as_bytes());
    msg.resize(total, 0);
    msg
}

/// Netlink attributes in `buf`: (type without flags, payload). Stops at the first malformed one.
fn attrs(buf: &[u8]) -> Vec<(u16, &[u8])> {
    let mut out = Vec::new();
    let mut rest = buf;
    while rest.len() >= 4 {
        let len = usize::from(u16::from_ne_bytes([rest[0], rest[1]]));
        let kind = u16::from_ne_bytes([rest[2], rest[3]]) & NLA_TYPE_MASK;
        if len < 4 || len > rest.len() {
            break;
        }
        out.push((kind, &rest[4..len]));
        rest = &rest[len.next_multiple_of(4).min(rest.len())..];
    }
    out
}

/// The id of multicast group `group` in a `CTRL_CMD_GETFAMILY` reply (one netlink message).
pub fn mcast_group_id(reply: &[u8], group: &str) -> Result<u32> {
    if reply.len() < 20 {
        bail!("short netlink reply");
    }
    if u16::from_ne_bytes([reply[4], reply[5]]) == NLMSG_ERROR {
        bail!("generic netlink has no such family");
    }
    let len = usize::try_from(u32::from_ne_bytes([reply[0], reply[1], reply[2], reply[3]]))?.min(reply.len());
    for (kind, groups) in attrs(&reply[20..len]) {
        if kind != CTRL_ATTR_MCAST_GROUPS {
            continue;
        }
        for (_, entry) in attrs(groups) {
            let fields = attrs(entry);
            let name = fields.iter().find(|(k, _)| *k == CTRL_ATTR_MCAST_GRP_NAME).map(|(_, v)| *v);
            let id = fields.iter().find(|(k, _)| *k == CTRL_ATTR_MCAST_GRP_ID).map(|(_, v)| *v);
            if let (Some(name), Some(id)) = (name, id)
                && name.strip_suffix(&[0]).unwrap_or(name) == group.as_bytes()
                && id.len() == 4
            {
                return Ok(u32::from_ne_bytes([id[0], id[1], id[2], id[3]]));
            }
        }
    }
    Err(anyhow!("no multicast group {group:?}"))
}

/// Joins the thermal `event` group; a thread blocks on the socket and wakes `notify` per message.
fn listen(notify: Arc<Notify>) -> Result<()> {
    let mut socket = Socket::new(NETLINK_GENERIC).context("netlink socket")?;
    socket.bind_auto().context("netlink bind")?;
    socket.send(&getfamily_request("thermal"), 0).context("netlink send")?;
    let (reply, _) = socket.recv_from_full().context("netlink recv")?;
    let group = mcast_group_id(&reply, "event")?;
    socket.add_membership(group).context("join thermal event group")?;
    std::thread::Builder::new().name("thermal-netlink".into()).spawn(move || {
        while socket.recv_from_full().is_ok() {
            notify.notify_one();
        }
        log::warn!("thermal: netlink socket closed");
    })?;
    Ok(())
}

async fn publish(conn: &Connection, zones: Vec<Zone>) -> zbus::Result<()> {
    let iface: InterfaceRef<Thermal> = conn.object_server().interface(PATH).await?;
    let (old_throttled, changed) = {
        let mut thermal = iface.get_mut().await;
        let old_throttled = thermal.zones.iter().any(|z| z.2 != "normal");
        let changed = zones_differ(&thermal.zones, &zones);
        thermal.zones = zones;
        (old_throttled, changed)
    };
    let thermal = iface.get().await;
    if changed {
        thermal.zones_changed(iface.signal_emitter()).await?;
    }
    if thermal.zones.iter().any(|z| z.2 != "normal") != old_throttled {
        thermal.throttled_changed(iface.signal_emitter()).await?;
    }
    Ok(())
}

fn zones_differ(a: &[Zone], b: &[Zone]) -> bool {
    a.len() != b.len()
        || a.iter()
            .zip(b)
            .any(|(x, y)| x.0 != y.0 || x.2 != y.2 || x.1.to_bits() != y.1.to_bits() || x.3.to_bits() != y.3.to_bits())
}

pub async fn run(conn: Connection) {
    let events = Arc::new(Notify::new());
    let reason = match listen(events.clone()) {
        Ok(()) => String::new(),
        Err(e) => {
            log::warn!("thermal: no netlink events ({e:#}); zones refresh only while watched");
            format!("no thermal netlink events: {e:#}")
        }
    };
    let mut watched = manager::watched();
    let mut tick = tokio::time::interval(Duration::from_secs(1));
    tick.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Delay);
    loop {
        let zones = read_zones();
        let available = !zones.is_empty();
        let why = if available { reason.as_str() } else { "no thermal zones in sysfs" };
        if let Err(e) = publish(&conn, zones).await.and(manager::set_area(&conn, AREA, available, why).await) {
            log::warn!("thermal: {e}");
        }
        let sampling = watched.borrow_and_update().contains(AREA);
        tokio::select! {
            () = events.notified() => {}
            _ = tick.tick(), if sampling => {}
            r = watched.changed() => if r.is_err() { return },
        }
    }
}

#[cfg(test)]
mod tests {
    use super::{mcast_group_id, zone_state};

    fn attr(kind: u16, payload: &[u8]) -> Vec<u8> {
        let len = u16::try_from(4 + payload.len()).unwrap_or(0);
        let mut out = [len.to_ne_bytes(), kind.to_ne_bytes()].concat();
        out.extend_from_slice(payload);
        out.resize(out.len().next_multiple_of(4), 0);
        out
    }

    #[test]
    fn finds_the_event_group_among_several() {
        let group = |name: &str, id: u32| {
            let mut name_z = name.as_bytes().to_vec();
            name_z.push(0);
            [attr(1, &name_z), attr(2, &id.to_ne_bytes())].concat()
        };
        let groups = attr(
            7 | 0x8000,
            &[attr(1 | 0x8000, &group("sampling", 11)), attr(2 | 0x8000, &group("event", 12))].concat(),
        );
        let body = [attr(1, &29u16.to_ne_bytes()), groups].concat();
        let total = u32::try_from(20 + body.len()).unwrap_or(0);
        let mut reply = [total.to_ne_bytes().as_slice(), &0x10u16.to_ne_bytes(), &[0; 10], &[1, 2, 0, 0]].concat();
        reply.extend_from_slice(&body);
        assert_eq!(mcast_group_id(&reply, "event").ok(), Some(12));
    }

    #[test]
    fn passive_trip_counts_but_active_does_not() {
        let trips = vec![("active".to_owned(), 50.0), ("passive".to_owned(), 90.0), ("critical".to_owned(), 105.0)];
        assert_eq!(zone_state(60.0, &trips).0, "normal");
        let (state, next) = zone_state(95.0, &trips);
        assert_eq!((state.as_str(), next), ("passive", 105.0));
    }
}
