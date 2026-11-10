//! The `network.wifi_power_save` actuator (docs/system-health.md §5): nl80211 power save on each Wi-Fi interface, as
//! `iw dev <if> set power_save` does. NetworkManager cannot reapply power save to a live connection.
//! Also the kernel side of the hotspot's access-point interface beside a Wi-Fi client, and the record of the
//! interfaces this service made (network.rs creates them through `wpa_supplicant`).

use std::fmt::Write as _;
use std::path::PathBuf;
use std::sync::{Mutex, PoisonError};

use anyhow::{Context, Result, anyhow, bail};
use netlink_sys::{
    Socket,
    protocols::{NETLINK_GENERIC, NETLINK_ROUTE},
};

use crate::thermal::{NLMSG_ERROR, attrs, getfamily_request};

const NLM_F_REQUEST: u16 = 1;
const NLM_F_ACK: u16 = 4;
const CTRL_ATTR_FAMILY_ID: u16 = 1;
const NL80211_CMD_SET_POWER_SAVE: u8 = 61;
const NL80211_CMD_GET_POWER_SAVE: u8 = 62;
const NL80211_ATTR_IFINDEX: u16 = 3;
const NL80211_ATTR_PS_STATE: u16 = 93;
const NLM_F_DUMP: u16 = 0x300;
const NLMSG_DONE: u16 = 3;
const NL80211_CMD_GET_WIPHY: u8 = 1;
const NL80211_CMD_GET_INTERFACE: u8 = 5;
const NL80211_CMD_SET_INTERFACE: u8 = 6;
const NL80211_CMD_NEW_INTERFACE: u8 = 7;
const NL80211_CMD_DEL_INTERFACE: u8 = 8;
const NL80211_ATTR_WIPHY: u16 = 1;
const NL80211_ATTR_IFNAME: u16 = 4;
const NL80211_ATTR_IFTYPE: u16 = 5;
const NL80211_ATTR_INTERFACE_COMBINATIONS: u16 = 120;
const NL80211_ATTR_SPLIT_WIPHY_DUMP: u16 = 174;
const NL80211_IFACE_COMB_LIMITS: u16 = 1;
const NL80211_IFACE_COMB_MAXNUM: u16 = 2;
const NL80211_IFACE_LIMIT_MAX: u16 = 1;
const NL80211_IFACE_LIMIT_TYPES: u16 = 2;
const NL80211_IFTYPE_STATION: u16 = 2;
const NL80211_IFTYPE_AP: u16 = 3;
const NL80211_ATTR_WIPHY_FREQ: u16 = 38;
const NL80211_ATTR_WIPHY_BANDS: u16 = 22;
const NL80211_BAND_ATTR_FREQS: u16 = 1;
const NL80211_FREQUENCY_ATTR_FREQ: u16 = 1;
const NL80211_FREQUENCY_ATTR_DISABLED: u16 = 2;
const NL80211_FREQUENCY_ATTR_NO_IR: u16 = 3;
const NL80211_FREQUENCY_ATTR_RADAR: u16 = 5;
/// IFNAMSIZ without the NUL.
const IFNAME_MAX: usize = 15;

struct Nl80211 {
    socket: Socket,
    family: u16,
}

fn u32_attr(kind: u16, value: u32) -> [u8; 8] {
    let mut attr = [0; 8];
    attr[0..2].copy_from_slice(&8u16.to_ne_bytes());
    attr[2..4].copy_from_slice(&kind.to_ne_bytes());
    attr[4..8].copy_from_slice(&value.to_ne_bytes());
    attr
}

/// The netlink error code of an `NLMSG_ERROR` reply (0 = ack), or `None` for another message.
fn error_code(reply: &[u8]) -> Option<i32> {
    (reply.len() >= 20 && u16::from_ne_bytes([reply[4], reply[5]]) == NLMSG_ERROR)
        .then(|| i32::from_ne_bytes([reply[16], reply[17], reply[18], reply[19]]))
}

impl Nl80211 {
    fn open() -> Result<Self> {
        let mut socket = Socket::new(NETLINK_GENERIC).context("netlink socket")?;
        socket.bind_auto().context("netlink bind")?;
        socket.send(&getfamily_request("nl80211"), 0).context("netlink send")?;
        let (reply, _) = socket.recv_from_full().context("netlink recv")?;
        if reply.len() < 20 || error_code(&reply).is_some() {
            bail!("no nl80211 (no Wi-Fi driver)");
        }
        let len = usize::try_from(u32::from_ne_bytes([reply[0], reply[1], reply[2], reply[3]]))?.min(reply.len());
        let family = attrs(&reply[20..len])
            .into_iter()
            .find(|(kind, value)| *kind == CTRL_ATTR_FAMILY_ID && value.len() == 2)
            .map(|(_, value)| u16::from_ne_bytes([value[0], value[1]]))
            .ok_or_else(|| anyhow!("nl80211 family id missing"))?;
        Ok(Self { socket, family })
    }

    fn send(&self, cmd: u8, flags: u16, attributes: &[&[u8]]) -> Result<()> {
        let total = 20 + attributes.iter().map(|a| a.len()).sum::<usize>();
        let mut msg = Vec::with_capacity(total);
        msg.extend_from_slice(&u32::try_from(total)?.to_ne_bytes());
        msg.extend_from_slice(&self.family.to_ne_bytes());
        msg.extend_from_slice(&(NLM_F_REQUEST | flags).to_ne_bytes());
        msg.extend_from_slice(&1u32.to_ne_bytes());
        msg.extend_from_slice(&0u32.to_ne_bytes());
        msg.extend_from_slice(&[cmd, 0, 0, 0]);
        for attr in attributes {
            msg.extend_from_slice(attr);
        }
        self.socket.send(&msg, 0).context("netlink send")?;
        Ok(())
    }

    fn request(&self, cmd: u8, flags: u16, attributes: &[&[u8]]) -> Result<Vec<u8>> {
        self.send(cmd, flags, attributes)?;
        let (reply, _) = self.socket.recv_from_full().context("netlink recv")?;
        match error_code(&reply) {
            Some(0) | None => Ok(reply),
            Some(code) => Err(std::io::Error::from_raw_os_error(-code).into()),
        }
    }

    /// The attributes of each message of a dump reply.
    fn dump(&self, cmd: u8, attributes: &[&[u8]]) -> Result<Vec<Vec<u8>>> {
        self.send(cmd, NLM_F_DUMP, attributes)?;
        let mut out = Vec::new();
        loop {
            let (reply, _) = self.socket.recv_from_full().context("netlink recv")?;
            let mut rest = reply.as_slice();
            while rest.len() >= 20 {
                let len = usize::try_from(u32::from_ne_bytes([rest[0], rest[1], rest[2], rest[3]]))?;
                if len < 20 || len > rest.len() {
                    bail!("malformed netlink reply");
                }
                match u16::from_ne_bytes([rest[4], rest[5]]) {
                    NLMSG_DONE => return Ok(out),
                    NLMSG_ERROR => match error_code(rest) {
                        Some(0) => {}
                        Some(code) => return Err(std::io::Error::from_raw_os_error(-code).into()),
                        None => bail!("malformed netlink error"),
                    },
                    _ => out.push(rest[20..len].to_vec()),
                }
                rest = &rest[len.next_multiple_of(4).min(rest.len())..];
            }
        }
    }

    /// The attributes of a reply that is one message.
    fn reply_attrs(reply: &[u8]) -> Result<Vec<(u16, &[u8])>> {
        if reply.len() < 20 {
            bail!("short netlink reply");
        }
        let len = usize::try_from(u32::from_ne_bytes([reply[0], reply[1], reply[2], reply[3]]))?.min(reply.len());
        Ok(attrs(reply.get(20..len).unwrap_or_default()))
    }

    fn power_save(&self, ifindex: u32) -> Result<bool> {
        let reply = self.request(NL80211_CMD_GET_POWER_SAVE, 0, &[&u32_attr(NL80211_ATTR_IFINDEX, ifindex)])?;
        Self::reply_attrs(&reply)?
            .into_iter()
            .find(|(kind, value)| *kind == NL80211_ATTR_PS_STATE && value.len() == 4)
            .map(|(_, value)| u32::from_ne_bytes([value[0], value[1], value[2], value[3]]) != 0)
            .ok_or_else(|| anyhow!("no power save state"))
    }

    fn set_power_save(&self, ifindex: u32, on: bool) -> Result<()> {
        let attributes: [&[u8]; 2] =
            [&u32_attr(NL80211_ATTR_IFINDEX, ifindex), &u32_attr(NL80211_ATTR_PS_STATE, u32::from(on))];
        self.request(NL80211_CMD_SET_POWER_SAVE, NLM_F_ACK, &attributes).map(drop)
    }

    fn iface(&self, ifindex: u32) -> Result<Iface> {
        let reply = self.request(NL80211_CMD_GET_INTERFACE, 0, &[&u32_attr(NL80211_ATTR_IFINDEX, ifindex)])?;
        iface_of(&Self::reply_attrs(&reply)?).ok_or_else(|| anyhow!("no wiphy or type for interface {ifindex}"))
    }

    /// The attributes of each message of the split `GET_WIPHY` dump of card `wiphy`.
    fn wiphy_dump(&self, wiphy: u32) -> Result<Vec<Vec<u8>>> {
        let mut split = [0; 4];
        split[0..2].copy_from_slice(&4u16.to_ne_bytes());
        split[2..4].copy_from_slice(&NL80211_ATTR_SPLIT_WIPHY_DUMP.to_ne_bytes());
        self.dump(NL80211_CMD_GET_WIPHY, &[&u32_attr(NL80211_ATTR_WIPHY, wiphy), &split])
    }

    fn set_ap_type(&self, ifindex: u32) -> Result<()> {
        let attributes: [&[u8]; 2] =
            [&u32_attr(NL80211_ATTR_IFINDEX, ifindex), &u32_attr(NL80211_ATTR_IFTYPE, u32::from(NL80211_IFTYPE_AP))];
        self.request(NL80211_CMD_SET_INTERFACE, NLM_F_ACK, &attributes).map(drop)
    }

    /// The ifindex of the new interface.
    fn new_ap_interface(&self, wiphy: u32, name: &str) -> Result<u32> {
        let mut ifname = Vec::with_capacity(4 + name.len() + 4);
        ifname.extend_from_slice(&u16::try_from(4 + name.len() + 1)?.to_ne_bytes());
        ifname.extend_from_slice(&NL80211_ATTR_IFNAME.to_ne_bytes());
        ifname.extend_from_slice(name.as_bytes());
        ifname.resize((4 + name.len() + 1).next_multiple_of(4), 0);
        let attributes: [&[u8]; 3] = [
            &u32_attr(NL80211_ATTR_WIPHY, wiphy),
            &ifname,
            &u32_attr(NL80211_ATTR_IFTYPE, u32::from(NL80211_IFTYPE_AP)),
        ];
        let reply = self.request(NL80211_CMD_NEW_INTERFACE, 0, &attributes)?;
        Self::reply_attrs(&reply)?
            .into_iter()
            .find_map(|(kind, value)| (kind == NL80211_ATTR_IFINDEX).then(|| u32_value(value)).flatten())
            .ok_or_else(|| anyhow!("no ifindex for {name}"))
    }

    fn del_interface(&self, ifindex: u32) -> Result<()> {
        self.request(NL80211_CMD_DEL_INTERFACE, NLM_F_ACK, &[&u32_attr(NL80211_ATTR_IFINDEX, ifindex)]).map(drop)
    }
}

/// Sets `IFF_UP` on interface `ifindex` (`ip link set <if> up`).
fn link_up(ifindex: u32) -> Result<()> {
    const RTM_NEWLINK: u16 = 16;
    const IFF_UP: u32 = 1;
    let mut socket = Socket::new(NETLINK_ROUTE).context("rtnetlink socket")?;
    socket.bind_auto().context("rtnetlink bind")?;
    let mut msg = Vec::with_capacity(32);
    msg.extend_from_slice(&32u32.to_ne_bytes());
    msg.extend_from_slice(&RTM_NEWLINK.to_ne_bytes());
    msg.extend_from_slice(&(NLM_F_REQUEST | NLM_F_ACK).to_ne_bytes());
    msg.extend_from_slice(&[1, 0, 0, 0, 0, 0, 0, 0]);
    // struct ifinfomsg: family, pad, type, index, flags, change.
    msg.extend_from_slice(&[0, 0, 0, 0]);
    msg.extend_from_slice(&i32::try_from(ifindex)?.to_ne_bytes());
    msg.extend_from_slice(&IFF_UP.to_ne_bytes());
    msg.extend_from_slice(&IFF_UP.to_ne_bytes());
    socket.send(&msg, 0).context("rtnetlink send")?;
    let (reply, _) = socket.recv_from_full().context("rtnetlink recv")?;
    match error_code(&reply) {
        Some(0) => Ok(()),
        Some(code) => Err(std::io::Error::from_raw_os_error(-code).into()),
        None => bail!("no rtnetlink ack"),
    }
}

fn u32_value(value: &[u8]) -> Option<u32> {
    Some(u32::from_ne_bytes(value.get(..4)?.try_into().ok()?))
}

fn combinations_in(wiphy: &[Vec<u8>]) -> bool {
    wiphy
        .iter()
        .flat_map(|m| attrs(m))
        .any(|(kind, value)| kind == NL80211_ATTR_INTERFACE_COMBINATIONS && combinations_allow_ap_with_station(value))
}

/// Whether the card whose `GET_WIPHY` dump is `wiphy` may start an access point on frequency `mhz` (MHz). cfg80211
/// relaxes NO-IR only for stations and P2P-GO (`cfg80211_ir_permissive_chan`), so an access point beside a client
/// on a NO-IR, radar or disabled channel fails to start.
fn ap_allowed_on(wiphy: &[Vec<u8>], mhz: u32) -> bool {
    let channels = wiphy
        .iter()
        .flat_map(|m| attrs(m))
        .filter(|(kind, _)| *kind == NL80211_ATTR_WIPHY_BANDS)
        .flat_map(|(_, bands)| attrs(bands))
        .flat_map(|(_, band)| attrs(band))
        .filter(|(kind, _)| *kind == NL80211_BAND_ATTR_FREQS)
        .flat_map(|(_, freqs)| attrs(freqs))
        .map(|(_, freq)| attrs(freq));
    let mut matching = channels.filter(|freq| {
        freq.iter().any(|(kind, value)| *kind == NL80211_FREQUENCY_ATTR_FREQ && u32_value(value) == Some(mhz))
    });
    matching.next().is_some_and(|freq| {
        !freq.iter().any(|(kind, _)| {
            matches!(
                *kind,
                NL80211_FREQUENCY_ATTR_DISABLED | NL80211_FREQUENCY_ATTR_NO_IR | NL80211_FREQUENCY_ATTR_RADAR
            )
        })
    })
}

/// Whether one entry of `NL80211_ATTR_INTERFACE_COMBINATIONS` (the attribute's payload) allows a station and an
/// access point at the same time. A shared channel limit is fine: the access point takes the station's channel.
fn combinations_allow_ap_with_station(combinations: &[u8]) -> bool {
    attrs(combinations).into_iter().any(|(_, combination)| {
        let fields = attrs(combination);
        let total = fields.iter().find(|(kind, _)| *kind == NL80211_IFACE_COMB_MAXNUM).and_then(|(_, v)| u32_value(v));
        let limits: Vec<(u32, Vec<u16>)> = fields
            .iter()
            .filter(|(kind, _)| *kind == NL80211_IFACE_COMB_LIMITS)
            .flat_map(|(_, limits)| attrs(limits))
            .map(|(_, limit)| {
                let limit = attrs(limit);
                let max = limit.iter().find(|(k, _)| *k == NL80211_IFACE_LIMIT_MAX).and_then(|(_, v)| u32_value(v));
                let types = limit
                    .iter()
                    .filter(|(k, _)| *k == NL80211_IFACE_LIMIT_TYPES)
                    .flat_map(|(_, types)| attrs(types).into_iter().map(|(iftype, _)| iftype))
                    .collect();
                (max.unwrap_or(0), types)
            })
            .collect();
        let fits = |station: usize, ap: usize| {
            let (station_max, _) = &limits[station];
            let (ap_max, _) = &limits[ap];
            if station == ap { *station_max >= 2 } else { *station_max >= 1 && *ap_max >= 1 }
        };
        total.unwrap_or(0) >= 2
            && (0..limits.len()).any(|s| {
                limits[s].1.contains(&NL80211_IFTYPE_STATION)
                    && (0..limits.len()).any(|a| limits[a].1.contains(&NL80211_IFTYPE_AP) && fits(s, a))
            })
    })
}

/// The card, nl80211 iftype and current frequency (MHz) of an interface.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Iface {
    wiphy: u32,
    iftype: u32,
    freq: Option<u32>,
}

/// The `Iface` in the attributes of a `GET_INTERFACE` reply.
fn iface_of(attributes: &[(u16, &[u8])]) -> Option<Iface> {
    let get = |want| attributes.iter().find_map(|(kind, value)| (*kind == want).then(|| u32_value(value)).flatten());
    Some(Iface {
        wiphy: get(NL80211_ATTR_WIPHY)?,
        iftype: get(NL80211_ATTR_IFTYPE)?,
        freq: get(NL80211_ATTR_WIPHY_FREQ),
    })
}

/// The kernel calls behind the access-point interface.
pub trait Kernel {
    /// The ifindex of each Wi-Fi interface.
    fn wifi_interfaces(&self) -> Vec<(String, u32)>;
    fn ifindex(&self, name: &str) -> Option<u32>;
    fn iface(&self, ifindex: u32) -> Result<Iface>;
    /// Whether card `wiphy` can run an access point while a station on it stays connected.
    fn ap_with_station(&self, wiphy: u32) -> Result<bool>;
    /// Whether card `wiphy` may start an access point on frequency `mhz`.
    fn ap_allowed_on(&self, wiphy: u32, mhz: u32) -> Result<bool>;
    /// Adds an access-point interface; returns its ifindex.
    fn new_ap(&self, wiphy: u32, name: &str) -> Result<u32>;
    fn set_ap_type(&self, ifindex: u32) -> Result<()>;
    fn link_up(&self, ifindex: u32) -> Result<()>;
    fn delete(&self, ifindex: u32) -> Result<()>;
}

struct Real(Nl80211);

impl Kernel for Real {
    fn wifi_interfaces(&self) -> Vec<(String, u32)> {
        interfaces()
    }
    fn ifindex(&self, name: &str) -> Option<u32> {
        ifindex(name)
    }
    fn iface(&self, ifindex: u32) -> Result<Iface> {
        self.0.iface(ifindex)
    }
    fn ap_with_station(&self, wiphy: u32) -> Result<bool> {
        Ok(combinations_in(&self.0.wiphy_dump(wiphy)?))
    }
    fn ap_allowed_on(&self, wiphy: u32, mhz: u32) -> Result<bool> {
        Ok(ap_allowed_on(&self.0.wiphy_dump(wiphy)?, mhz))
    }
    fn new_ap(&self, wiphy: u32, name: &str) -> Result<u32> {
        self.0.new_ap_interface(wiphy, name)
    }
    fn set_ap_type(&self, ifindex: u32) -> Result<()> {
        self.0.set_ap_type(ifindex)
    }
    fn link_up(&self, ifindex: u32) -> Result<()> {
        link_up(ifindex)
    }
    fn delete(&self, ifindex: u32) -> Result<()> {
        self.0.del_interface(ifindex)
    }
}

/// Interfaces in a text file, one `name ifindex wiphy iftype up freq` line each (freq 0: none), for test buses
/// and the cargo tests. Every card allows an access point on every channel.
struct Fake(PathBuf);

impl Fake {
    fn rows(&self) -> Vec<[u32; 5]> {
        self.named().into_iter().map(|(_, row)| row).collect()
    }
    fn named(&self) -> Vec<(String, [u32; 5])> {
        let text = std::fs::read_to_string(&self.0).unwrap_or_default();
        text.lines()
            .filter_map(|line| {
                let mut fields = line.split_whitespace();
                let name = fields.next()?.to_owned();
                let mut row = [0; 5];
                for slot in &mut row {
                    *slot = fields.next()?.parse().ok()?;
                }
                Some((name, row))
            })
            .collect()
    }
    fn edit(&self, f: impl FnOnce(&mut Vec<(String, [u32; 5])>)) -> Result<()> {
        let mut rows = self.named();
        f(&mut rows);
        let text = rows.iter().fold(String::new(), |mut text, (name, r)| {
            let _ = writeln!(text, "{name} {} {} {} {} {}", r[0], r[1], r[2], r[3], r[4]);
            text
        });
        std::fs::write(&self.0, text).context("fake interfaces")
    }
    fn set(&self, ifindex: u32, field: usize, value: u32) -> Result<()> {
        if !self.rows().iter().any(|r| r[0] == ifindex) {
            bail!("no interface {ifindex}");
        }
        self.edit(|rows| rows.iter_mut().filter(|(_, r)| r[0] == ifindex).for_each(|(_, r)| r[field] = value))
    }
}

impl Kernel for Fake {
    fn wifi_interfaces(&self) -> Vec<(String, u32)> {
        self.named().into_iter().map(|(name, r)| (name, r[0])).collect()
    }
    fn ifindex(&self, name: &str) -> Option<u32> {
        self.named().into_iter().find_map(|(n, r)| (n == name).then_some(r[0]))
    }
    fn iface(&self, ifindex: u32) -> Result<Iface> {
        let r = self.rows().into_iter().find(|r| r[0] == ifindex).ok_or_else(|| anyhow!("no interface {ifindex}"))?;
        Ok(Iface { wiphy: r[1], iftype: r[2], freq: (r[4] != 0).then_some(r[4]) })
    }
    fn ap_with_station(&self, _: u32) -> Result<bool> {
        Ok(true)
    }
    fn ap_allowed_on(&self, _: u32, _: u32) -> Result<bool> {
        Ok(true)
    }
    fn new_ap(&self, wiphy: u32, name: &str) -> Result<u32> {
        let ifindex = self.rows().iter().map(|r| r[0]).max().unwrap_or(0) + 1;
        self.edit(|rows| rows.push((name.to_owned(), [ifindex, wiphy, u32::from(NL80211_IFTYPE_AP), 0, 0])))?;
        Ok(ifindex)
    }
    fn set_ap_type(&self, ifindex: u32) -> Result<()> {
        self.set(ifindex, 2, u32::from(NL80211_IFTYPE_AP))
    }
    fn link_up(&self, ifindex: u32) -> Result<()> {
        self.set(ifindex, 3, 1)
    }
    fn delete(&self, ifindex: u32) -> Result<()> {
        self.edit(|rows| rows.retain(|(_, r)| r[0] != ifindex))
    }
}

/// nl80211, or the file `ANDROMEDA_SYSTEM_FAKE_WIFI` names on a test bus (`polkit::test_bus`; the unit sets neither).
pub fn kernel() -> Result<Box<dyn Kernel>> {
    if crate::polkit::test_bus()
        && let Some(path) = std::env::var_os("ANDROMEDA_SYSTEM_FAKE_WIFI")
    {
        return Ok(Box::new(Fake(path.into())));
    }
    Ok(Box::new(Real(Nl80211::open()?)))
}

/// One access-point interface this service made; it is valid only in the boot that made it.
#[derive(Clone, Debug, PartialEq)]
struct ApRecord {
    boot_id: String,
    ifindex: u32,
    name: String,
}

/// The access-point interfaces this service made, one `boot_id ifindex name` line each, in its `StateDirectory`, so
/// a restart of the service can still remove them.
pub struct Records {
    path: PathBuf,
    lock: Mutex<()>,
}

impl Records {
    pub fn new(path: PathBuf) -> Self {
        Self { path, lock: Mutex::new(()) }
    }

    fn load(&self) -> Vec<ApRecord> {
        let text = std::fs::read_to_string(&self.path).unwrap_or_default();
        text.lines()
            .filter_map(|line| {
                let mut fields = line.split_whitespace();
                let boot_id = fields.next()?.to_owned();
                let ifindex = fields.next()?.parse().ok()?;
                Some(ApRecord { boot_id, ifindex, name: fields.next()?.to_owned() })
            })
            .collect()
    }

    fn edit(&self, f: impl FnOnce(&mut Vec<ApRecord>)) -> Result<()> {
        let _held = self.lock.lock().unwrap_or_else(PoisonError::into_inner);
        let mut records = self.load();
        f(&mut records);
        let text = records.iter().fold(String::new(), |mut text, r| {
            let _ = writeln!(text, "{} {} {}", r.boot_id, r.ifindex, r.name);
            text
        });
        let tmp = self.path.with_extension("tmp");
        std::fs::write(&tmp, text).and_then(|()| std::fs::rename(&tmp, &self.path)).context("access-point records")
    }

    /// Records `name` (`ifindex`) as made in boot `boot_id`, and drops records of other boots.
    fn add(&self, boot_id: &str, ifindex: u32, name: &str) -> Result<()> {
        self.edit(|records| {
            records.retain(|r| r.boot_id == boot_id && r.name != name);
            records.push(ApRecord { boot_id: boot_id.to_owned(), ifindex, name: name.to_owned() });
        })
    }

    pub fn forget(&self, name: &str) -> Result<()> {
        self.edit(|records| records.retain(|r| r.name != name))
    }

    fn owns(&self, boot_id: &str, ifindex: u32, name: &str) -> bool {
        self.load().iter().any(|r| r.boot_id == boot_id && r.ifindex == ifindex && r.name == name)
    }
}

fn boot_id() -> Result<String> {
    Ok(std::fs::read_to_string("/proc/sys/kernel/random/boot_id").context("boot id")?.trim().to_owned())
}

/// Whether `ap` is named `ap_name(client)` for another Wi-Fi interface `client` on card `wiphy`.
fn on_card_of_client(ap: &str, wiphy: u32, wifi: &[(String, Iface)]) -> bool {
    wifi.iter().any(|(client, iface)| client != ap && ap_name(client) == ap && iface.wiphy == wiphy)
}

/// Whether access-point interface `ap` exists and this service made it.
pub enum Ownership {
    Absent,
    NotOurs,
    Ours(u32),
}

/// Callers that pass the polkit check may name any interface, so only a record this root service wrote in its own
/// `StateDirectory`, for this boot and the live ifindex (never reused within a boot), lets it delete one.
fn ownership(k: &dyn Kernel, records: &Records, boot_id: &str, ap: &str) -> Ownership {
    let Some(index) = k.ifindex(ap) else { return Ownership::Absent };
    let wifi: Vec<(String, Iface)> =
        k.wifi_interfaces().into_iter().filter_map(|(name, i)| Some((name, k.iface(i).ok()?))).collect();
    let on_card = k.iface(index).is_ok_and(|iface| on_card_of_client(ap, iface.wiphy, &wifi));
    if on_card && records.owns(boot_id, index, ap) { Ownership::Ours(index) } else { Ownership::NotOurs }
}

/// The name of the access-point interface beside `client`.
pub fn ap_name(client: &str) -> String {
    client.chars().take(IFNAME_MAX - 2).chain("ap".chars()).collect()
}

fn wifi_ifindex(k: &dyn Kernel, name: &str) -> Result<u32> {
    k.wifi_interfaces()
        .into_iter()
        .find_map(|(n, index)| (n == name).then_some(index))
        .ok_or_else(|| anyhow!("{name} is not a Wi-Fi interface"))
}

/// The current ifindex of interface `name`.
fn ifindex(name: &str) -> Option<u32> {
    std::fs::read_to_string(format!("/sys/class/net/{name}/ifindex")).ok()?.trim().parse().ok()
}

/// Whether an access point can run beside Wi-Fi interface `client` now: its card allows both at once and the
/// client's current channel allows an access point. False while `client` has no channel.
pub fn can_run_ap_with_client(k: &dyn Kernel, client: &str) -> Result<bool> {
    let iface = k.iface(wifi_ifindex(k, client)?)?;
    let Some(mhz) = iface.freq else { return Ok(false) };
    Ok(k.ap_with_station(iface.wiphy)? && k.ap_allowed_on(iface.wiphy, mhz)?)
}

/// The access-point interface beside a Wi-Fi client: its name, its card, and whether this service made it already.
pub struct ApPlan {
    pub name: String,
    pub wiphy: u32,
    pub exists: bool,
}

/// Plans the access-point interface beside Wi-Fi interface `client`. Fails when the card cannot run both at once or
/// when an interface that this service did not make has the name.
pub fn plan_ap(k: &dyn Kernel, records: &Records, client: &str) -> Result<ApPlan> {
    let wiphy = k.iface(wifi_ifindex(k, client)?)?.wiphy;
    if !k.ap_with_station(wiphy)? {
        bail!("this Wi-Fi card cannot run a hotspot while it stays connected");
    }
    let name = ap_name(client);
    let exists = match ownership(k, records, &boot_id()?, &name) {
        Ownership::Absent => false,
        Ownership::Ours(_) => true,
        Ownership::NotOurs => bail!("{name} already exists"),
    };
    Ok(ApPlan { name, wiphy, exists })
}

/// Records `name`, which this service just made or made before, and checks that it is on card `wiphy`; returns its
/// ifindex. The record comes first, so a removal after a failure here still finds it ours.
pub fn adopt_ap(k: &dyn Kernel, records: &Records, name: &str, wiphy: u32) -> Result<u32> {
    let ifindex = k.ifindex(name).ok_or_else(|| anyhow!("{name} did not appear"))?;
    records.add(&boot_id()?, ifindex, name)?;
    let card = k.iface(ifindex)?.wiphy;
    if card != wiphy {
        bail!("{name} is on Wi-Fi card {card}, not {wiphy}");
    }
    Ok(ifindex)
}

/// Makes interface `ifindex` an access point again and brings it up. NetworkManager sets every idle Wi-Fi interface
/// it manages to station mode, when it takes the interface over and after each deactivation.
pub fn arm(k: &dyn Kernel, ifindex: u32) -> Result<()> {
    if k.iface(ifindex)?.iftype != u32::from(NL80211_IFTYPE_AP) {
        k.set_ap_type(ifindex).context("set type AP")?;
    }
    k.link_up(ifindex).context("link up")
}

pub fn ownership_of(k: &dyn Kernel, records: &Records, ap: &str) -> Result<Ownership> {
    Ok(ownership(k, records, &boot_id()?, ap))
}

/// Deletes `ap` while it still has `ifindex` (`wpa_supplicant` may have deleted it already) and forgets it.
pub fn delete_ap(k: &dyn Kernel, records: &Records, ap: &str, ifindex: u32) -> Result<()> {
    if k.ifindex(ap) == Some(ifindex) {
        k.delete(ifindex).with_context(|| format!("{ap}: delete interface"))?;
    }
    records.forget(ap)
}

/// The ifindex of each Wi-Fi interface.
fn interfaces() -> Vec<(String, u32)> {
    let Ok(entries) = std::fs::read_dir("/sys/class/net") else { return Vec::new() };
    entries
        .flatten()
        .filter(|e| e.path().join("wireless").exists())
        .filter_map(|e| {
            let index = std::fs::read_to_string(e.path().join("ifindex")).ok()?.trim().parse().ok()?;
            Some((e.file_name().to_string_lossy().into_owned(), index))
        })
        .collect()
}

/// Whether power save is on for any Wi-Fi interface; `None` without one.
pub fn power_save() -> Result<Option<bool>> {
    let interfaces = interfaces();
    if interfaces.is_empty() {
        return Ok(None);
    }
    let nl = Nl80211::open()?;
    let mut any = false;
    for (name, index) in interfaces {
        any |= nl.power_save(index).with_context(|| format!("{name}: get power save"))?;
    }
    Ok(Some(any))
}

/// Sets power save on every Wi-Fi interface.
pub fn set_power_save(on: bool) -> Result<()> {
    let interfaces = interfaces();
    if interfaces.is_empty() {
        return Ok(());
    }
    let nl = Nl80211::open()?;
    for (name, index) in interfaces {
        nl.set_power_save(index, on).with_context(|| format!("{name}: set power save"))?;
    }
    Ok(())
}

#[cfg(test)]
#[allow(clippy::unwrap_used)]
mod tests {
    use super::*;

    // Payloads of NL80211_ATTR_INTERFACE_COMBINATIONS, little-endian, built by hand from the attribute layout of
    // nl80211_put_iface_combinations() (net/wireless/nl80211.c) because `iw` prints no bytes. Each combination is a
    // nest (NLA_F_NESTED) of IFACE_COMB_LIMITS (one nest per limit: LIMIT_MAX u32, LIMIT_TYPES with one flag per
    // iftype), then COMB_NUM_CHANNELS u32 and COMB_MAXNUM u32. THIS_MACHINE is `iw list` on the Intel card here:
    //   { managed } <= 2, { P2P-client, P2P-GO } <= 1, { P2P-device } <= 1, total <= 4, #channels <= 2
    //   { managed } <= 2, { AP, P2P-client, P2P-GO } <= 1, { P2P-device } <= 1, total <= 4, #channels <= 1
    //   { managed } <= 1, { NAN } <= 1, { iftype 13 } <= 2, total <= 4, #channels <= 3
    // WITHOUT_SECOND drops the second line. SHARED_LIMIT_OF_ONE: { managed, AP } <= 1, total <= 2.
    const THIS_MACHINE: &[u8] = &[
        0x58, 0x00, 0x01, 0x80, 0x44, 0x00, 0x01, 0x80, 0x14, 0x00, 0x01, 0x80, 0x08, 0x00, 0x01, 0x00, 0x02, 0x00,
        0x00, 0x00, 0x08, 0x00, 0x02, 0x80, 0x04, 0x00, 0x02, 0x00, 0x18, 0x00, 0x02, 0x80, 0x08, 0x00, 0x01, 0x00,
        0x01, 0x00, 0x00, 0x00, 0x0c, 0x00, 0x02, 0x80, 0x04, 0x00, 0x08, 0x00, 0x04, 0x00, 0x09, 0x00, 0x14, 0x00,
        0x03, 0x80, 0x08, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x08, 0x00, 0x02, 0x80, 0x04, 0x00, 0x0a, 0x00,
        0x08, 0x00, 0x04, 0x00, 0x02, 0x00, 0x00, 0x00, 0x08, 0x00, 0x02, 0x00, 0x04, 0x00, 0x00, 0x00, 0x5c, 0x00,
        0x02, 0x80, 0x48, 0x00, 0x01, 0x80, 0x14, 0x00, 0x01, 0x80, 0x08, 0x00, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00,
        0x08, 0x00, 0x02, 0x80, 0x04, 0x00, 0x02, 0x00, 0x1c, 0x00, 0x02, 0x80, 0x08, 0x00, 0x01, 0x00, 0x01, 0x00,
        0x00, 0x00, 0x10, 0x00, 0x02, 0x80, 0x04, 0x00, 0x03, 0x00, 0x04, 0x00, 0x08, 0x00, 0x04, 0x00, 0x09, 0x00,
        0x14, 0x00, 0x03, 0x80, 0x08, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x08, 0x00, 0x02, 0x80, 0x04, 0x00,
        0x0a, 0x00, 0x08, 0x00, 0x04, 0x00, 0x01, 0x00, 0x00, 0x00, 0x08, 0x00, 0x02, 0x00, 0x04, 0x00, 0x00, 0x00,
        0x54, 0x00, 0x03, 0x80, 0x40, 0x00, 0x01, 0x80, 0x14, 0x00, 0x01, 0x80, 0x08, 0x00, 0x01, 0x00, 0x01, 0x00,
        0x00, 0x00, 0x08, 0x00, 0x02, 0x80, 0x04, 0x00, 0x02, 0x00, 0x14, 0x00, 0x02, 0x80, 0x08, 0x00, 0x01, 0x00,
        0x01, 0x00, 0x00, 0x00, 0x08, 0x00, 0x02, 0x80, 0x04, 0x00, 0x0c, 0x00, 0x14, 0x00, 0x03, 0x80, 0x08, 0x00,
        0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0x08, 0x00, 0x02, 0x80, 0x04, 0x00, 0x0d, 0x00, 0x08, 0x00, 0x04, 0x00,
        0x03, 0x00, 0x00, 0x00, 0x08, 0x00, 0x02, 0x00, 0x04, 0x00, 0x00, 0x00,
    ];
    const WITHOUT_SECOND: &[u8] = &[
        0x58, 0x00, 0x01, 0x80, 0x44, 0x00, 0x01, 0x80, 0x14, 0x00, 0x01, 0x80, 0x08, 0x00, 0x01, 0x00, 0x02, 0x00,
        0x00, 0x00, 0x08, 0x00, 0x02, 0x80, 0x04, 0x00, 0x02, 0x00, 0x18, 0x00, 0x02, 0x80, 0x08, 0x00, 0x01, 0x00,
        0x01, 0x00, 0x00, 0x00, 0x0c, 0x00, 0x02, 0x80, 0x04, 0x00, 0x08, 0x00, 0x04, 0x00, 0x09, 0x00, 0x14, 0x00,
        0x03, 0x80, 0x08, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x08, 0x00, 0x02, 0x80, 0x04, 0x00, 0x0a, 0x00,
        0x08, 0x00, 0x04, 0x00, 0x02, 0x00, 0x00, 0x00, 0x08, 0x00, 0x02, 0x00, 0x04, 0x00, 0x00, 0x00, 0x54, 0x00,
        0x02, 0x80, 0x40, 0x00, 0x01, 0x80, 0x14, 0x00, 0x01, 0x80, 0x08, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00,
        0x08, 0x00, 0x02, 0x80, 0x04, 0x00, 0x02, 0x00, 0x14, 0x00, 0x02, 0x80, 0x08, 0x00, 0x01, 0x00, 0x01, 0x00,
        0x00, 0x00, 0x08, 0x00, 0x02, 0x80, 0x04, 0x00, 0x0c, 0x00, 0x14, 0x00, 0x03, 0x80, 0x08, 0x00, 0x01, 0x00,
        0x02, 0x00, 0x00, 0x00, 0x08, 0x00, 0x02, 0x80, 0x04, 0x00, 0x0d, 0x00, 0x08, 0x00, 0x04, 0x00, 0x03, 0x00,
        0x00, 0x00, 0x08, 0x00, 0x02, 0x00, 0x04, 0x00, 0x00, 0x00,
    ];
    const SHARED_LIMIT_OF_ONE: &[u8] = &[
        0x30, 0x00, 0x01, 0x80, 0x1c, 0x00, 0x01, 0x80, 0x18, 0x00, 0x01, 0x80, 0x08, 0x00, 0x01, 0x00, 0x01, 0x00,
        0x00, 0x00, 0x0c, 0x00, 0x02, 0x80, 0x04, 0x00, 0x02, 0x00, 0x04, 0x00, 0x03, 0x00, 0x08, 0x00, 0x04, 0x00,
        0x01, 0x00, 0x00, 0x00, 0x08, 0x00, 0x02, 0x00, 0x02, 0x00, 0x00, 0x00,
    ];

    #[test]
    fn combination_parser_finds_ap_beside_station() {
        assert!(combinations_allow_ap_with_station(THIS_MACHINE));
        assert!(!combinations_allow_ap_with_station(WITHOUT_SECOND));
        assert!(!combinations_allow_ap_with_station(SHARED_LIMIT_OF_ONE));
    }

    // A GET_INTERFACE reply's attributes as `iw dev wlan0ap info` shows them: wiphy 0, ifindex 9, type AP.
    const AP_REPLY: &[u8] = &[
        0x08, 0x00, 0x03, 0x00, 0x09, 0x00, 0x00, 0x00, 0x0c, 0x00, 0x04, 0x00, b'w', b'l', b'a', b'n', b'0', b'a',
        b'p', 0x00, 0x08, 0x00, 0x05, 0x00, 0x03, 0x00, 0x00, 0x00, 0x08, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
    ];

    #[test]
    fn get_interface_reply_parses() {
        let ap = Iface { wiphy: 0, iftype: u32::from(NL80211_IFTYPE_AP), freq: None };
        assert_eq!(iface_of(&attrs(AP_REPLY)), Some(ap));
    }

    fn nla(kind: u16, payload: &[u8]) -> Vec<u8> {
        let mut out = Vec::new();
        out.extend_from_slice(&u16::try_from(4 + payload.len()).unwrap().to_ne_bytes());
        out.extend_from_slice(&kind.to_ne_bytes());
        out.extend_from_slice(payload);
        out.resize(out.len().next_multiple_of(4), 0);
        out
    }

    /// One `GET_WIPHY` dump message with one band of channels, each a frequency and its flag attributes.
    fn bands(channels: &[(u32, &[u16])]) -> Vec<u8> {
        let freqs: Vec<u8> = channels
            .iter()
            .enumerate()
            .flat_map(|(i, (mhz, flags))| {
                let mut entry = nla(NL80211_FREQUENCY_ATTR_FREQ, &mhz.to_ne_bytes());
                flags.iter().for_each(|flag| entry.extend(nla(*flag, &[])));
                nla(u16::try_from(i).unwrap() | 0x8000, &entry)
            })
            .collect();
        let band = nla(0x8000 | 1, &nla(0x8000 | NL80211_BAND_ATTR_FREQS, &freqs));
        nla(0x8000 | NL80211_ATTR_WIPHY_BANDS, &band)
    }

    #[test]
    fn an_access_point_shares_only_a_channel_it_may_start_on() {
        let dump = vec![
            bands(&[(2412, &[]), (5180, &[NL80211_FREQUENCY_ATTR_NO_IR])]),
            bands(&[(5260, &[NL80211_FREQUENCY_ATTR_RADAR]), (5745, &[]), (5825, &[NL80211_FREQUENCY_ATTR_DISABLED])]),
        ];
        assert!(ap_allowed_on(&dump, 2412));
        assert!(ap_allowed_on(&dump, 5745), "a channel in a later message of the split dump");
        assert!(!ap_allowed_on(&dump, 5180), "NO-IR allows stations only");
        assert!(!ap_allowed_on(&dump, 5260), "radar");
        assert!(!ap_allowed_on(&dump, 5825), "disabled");
        assert!(!ap_allowed_on(&dump, 5500), "a channel the card does not list");
    }

    /// A fake kernel with client wlan0 (ifindex 3, card 0, on 2412 MHz) and records, both in fresh files.
    fn setup(test: &str) -> (Fake, Records) {
        let dir = std::env::temp_dir().join(format!("andromeda-wifi-{test}-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        std::fs::write(dir.join("net"), "wlan0 3 0 2 1 2412\n").unwrap();
        let _ = std::fs::remove_file(dir.join("records"));
        (Fake(dir.join("net")), Records::new(dir.join("records")))
    }

    #[test]
    fn only_a_recorded_interface_of_this_boot_is_ours() {
        let (k, records) = setup("ownership");
        assert!(matches!(ownership(&k, &records, "boot-a", "wlan0ap"), Ownership::Absent));
        let index = k.new_ap(0, "wlan0ap").unwrap();
        assert!(matches!(ownership(&k, &records, "boot-a", "wlan0ap"), Ownership::NotOurs), "made by someone else");
        records.add("boot-a", index, "wlan0ap").unwrap();
        assert!(matches!(ownership(&k, &records, "boot-a", "wlan0ap"), Ownership::Ours(i) if i == index));
        assert!(matches!(ownership(&k, &records, "boot-b", "wlan0ap"), Ownership::NotOurs), "a record of another boot");
        k.delete(index).unwrap();
        // The kernel never hands out an ifindex twice in a boot; the fake would.
        let other = 7;
        k.set(k.new_ap(0, "wlan0ap").unwrap(), 0, other).unwrap();
        assert!(
            matches!(ownership(&k, &records, "boot-a", "wlan0ap"), Ownership::NotOurs),
            "a new interface, same name"
        );
        records.add("boot-a", other, "wlan0ap").unwrap();
        k.set(other, 1, 1).unwrap();
        assert!(matches!(ownership(&k, &records, "boot-a", "wlan0ap"), Ownership::NotOurs), "on another card");
        records.add("boot-b", 9, "wlan1ap").unwrap();
        assert_eq!(records.load(), vec![ApRecord { boot_id: "boot-b".into(), ifindex: 9, name: "wlan1ap".into() }]);
        records.forget("wlan1ap").unwrap();
        assert!(records.load().is_empty());
    }

    #[test]
    fn a_second_create_rearms_the_same_interface() {
        let (k, records) = setup("rearm");
        let plan = plan_ap(&k, &records, "wlan0").unwrap();
        assert!(!plan.exists);
        let index = k.new_ap(plan.wiphy, &plan.name).unwrap();
        assert_eq!(adopt_ap(&k, &records, &plan.name, plan.wiphy).unwrap(), index);
        // NetworkManager sets the idle interface to station mode.
        k.set(index, 2, u32::from(NL80211_IFTYPE_STATION)).unwrap();
        let again = plan_ap(&k, &records, "wlan0").unwrap();
        assert!(again.exists && again.name == "wlan0ap");
        arm(&k, index).unwrap();
        assert_eq!(k.iface(index).unwrap().iftype, u32::from(NL80211_IFTYPE_AP));
        assert_eq!(k.rows().iter().find(|r| r[0] == index).unwrap()[3], 1, "link up");
        records.forget("wlan0ap").unwrap();
        assert!(plan_ap(&k, &records, "wlan0").is_err(), "an interface this service did not make keeps its name");
    }

    #[test]
    fn no_channel_no_access_point_beside_the_client() {
        let (k, _) = setup("channel");
        assert!(can_run_ap_with_client(&k, "wlan0").unwrap());
        k.set(3, 4, 0).unwrap();
        assert!(!can_run_ap_with_client(&k, "wlan0").unwrap());
    }

    #[test]
    fn ap_name_fits_ifnamsiz() {
        assert_eq!(ap_name("wlan0"), "wlan0ap");
        assert_eq!(ap_name("wlp0s20f3abcdefgh").len(), IFNAME_MAX);
    }
}
