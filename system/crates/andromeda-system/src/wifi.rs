//! The `network.wifi_power_save` actuator (docs/system-health.md §5): nl80211 power save on each Wi-Fi interface, as
//! `iw dev <if> set power_save` does. NetworkManager cannot reapply power save to a live connection.
//! Also the hotspot's access-point interface beside a Wi-Fi client (`iw phy <phy> interface add <name> type __ap`).

use anyhow::{Context, Result, anyhow, bail};
use netlink_sys::{Socket, protocols::NETLINK_GENERIC};

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

    fn wiphy(&self, ifindex: u32) -> Result<u32> {
        Ok(self.iface(ifindex)?.wiphy)
    }

    fn ap_with_station(&self, wiphy: u32) -> Result<bool> {
        let mut split = [0; 4];
        split[0..2].copy_from_slice(&4u16.to_ne_bytes());
        split[2..4].copy_from_slice(&NL80211_ATTR_SPLIT_WIPHY_DUMP.to_ne_bytes());
        let messages = self.dump(NL80211_CMD_GET_WIPHY, &[&u32_attr(NL80211_ATTR_WIPHY, wiphy), &split])?;
        Ok(messages.iter().flat_map(|m| attrs(m)).any(|(kind, value)| {
            kind == NL80211_ATTR_INTERFACE_COMBINATIONS && combinations_allow_ap_with_station(value)
        }))
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

fn u32_value(value: &[u8]) -> Option<u32> {
    Some(u32::from_ne_bytes(value.get(..4)?.try_into().ok()?))
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

/// The card and nl80211 iftype of an interface.
#[derive(Clone, Copy, Debug, PartialEq)]
struct Iface {
    wiphy: u32,
    iftype: u32,
}

/// The `Iface` in the attributes of a `GET_INTERFACE` reply.
fn iface_of(attributes: &[(u16, &[u8])]) -> Option<Iface> {
    let get = |want| attributes.iter().find_map(|(kind, value)| (*kind == want).then(|| u32_value(value)).flatten());
    Some(Iface { wiphy: get(NL80211_ATTR_WIPHY)?, iftype: get(NL80211_ATTR_IFTYPE)? })
}

/// Whether `ap` is an access-point interface of the kind `create_ap_interface` makes: type AP and named
/// `ap_name(client)` for another Wi-Fi interface `client` on the same card. No state survives a restart, so this is
/// the only proof of ownership.
fn is_ours(ap: &str, ap_iface: Iface, wifi: &[(String, Iface)]) -> bool {
    ap_iface.iftype == u32::from(NL80211_IFTYPE_AP)
        && wifi.iter().any(|(client, iface)| client != ap && ap_name(client) == ap && iface.wiphy == ap_iface.wiphy)
}

/// The ifindex of `ap` when `is_ours` holds for it now.
fn our_ap(nl: &Nl80211, ap: &str) -> Option<u32> {
    let index = ifindex(ap)?;
    let ap_iface = nl.iface(index).ok()?;
    let wifi: Vec<(String, Iface)> =
        interfaces().into_iter().filter_map(|(name, i)| Some((name, nl.iface(i).ok()?))).collect();
    is_ours(ap, ap_iface, &wifi).then_some(index)
}

/// The name of the access-point interface beside `client`.
pub fn ap_name(client: &str) -> String {
    client.chars().take(IFNAME_MAX - 2).chain("ap".chars()).collect()
}

fn wifi_ifindex(name: &str) -> Result<u32> {
    interfaces()
        .into_iter()
        .find_map(|(n, index)| (n == name).then_some(index))
        .ok_or_else(|| anyhow!("{name} is not a Wi-Fi interface"))
}

/// The current ifindex of interface `name`.
fn ifindex(name: &str) -> Option<u32> {
    std::fs::read_to_string(format!("/sys/class/net/{name}/ifindex")).ok()?.trim().parse().ok()
}

/// Whether the card of Wi-Fi interface `client` can run an access point while `client` stays connected.
pub fn can_run_ap_with_client(client: &str) -> Result<bool> {
    let nl = Nl80211::open()?;
    nl.ap_with_station(nl.wiphy(wifi_ifindex(client)?)?)
}

/// Adds an access-point interface on the card of Wi-Fi interface `client`, or finds the one that is already there;
/// returns its name and ifindex.
pub fn create_ap_interface(client: &str) -> Result<(String, u32)> {
    let index = wifi_ifindex(client)?;
    let nl = Nl80211::open()?;
    let wiphy = nl.wiphy(index)?;
    if !nl.ap_with_station(wiphy)? {
        bail!("this Wi-Fi card cannot run a hotspot while it stays connected");
    }
    let name = ap_name(client);
    if let Some(existing) = ifindex(&name) {
        if our_ap(&nl, &name) == Some(existing) {
            return Ok((name, existing));
        }
        bail!("{name} already exists");
    }
    let ap = nl.new_ap_interface(wiphy, &name).with_context(|| format!("{name}: new interface"))?;
    Ok((name, ap))
}

pub enum ApRemoval {
    Removed,
    Absent,
    /// The interface is not one that `create_ap_interface` makes; nothing changed.
    NotOurs,
}

/// Deletes access-point interface `ap` when `is_ours` holds for it.
pub fn remove_ap_interface(ap: &str) -> Result<ApRemoval> {
    if ifindex(ap).is_none() {
        return Ok(ApRemoval::Absent);
    }
    let nl = Nl80211::open()?;
    let Some(index) = our_ap(&nl, ap) else { return Ok(ApRemoval::NotOurs) };
    nl.del_interface(index)?;
    Ok(ApRemoval::Removed)
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
        0x58, 0x00, 0x01, 0x80, 0x44, 0x00, 0x01, 0x80, 0x14, 0x00, 0x01, 0x80, 0x08, 0x00, 0x01, 0x00,
        0x02, 0x00, 0x00, 0x00, 0x08, 0x00, 0x02, 0x80, 0x04, 0x00, 0x02, 0x00, 0x18, 0x00, 0x02, 0x80,
        0x08, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x0c, 0x00, 0x02, 0x80, 0x04, 0x00, 0x08, 0x00,
        0x04, 0x00, 0x09, 0x00, 0x14, 0x00, 0x03, 0x80, 0x08, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00,
        0x08, 0x00, 0x02, 0x80, 0x04, 0x00, 0x0a, 0x00, 0x08, 0x00, 0x04, 0x00, 0x02, 0x00, 0x00, 0x00,
        0x08, 0x00, 0x02, 0x00, 0x04, 0x00, 0x00, 0x00, 0x5c, 0x00, 0x02, 0x80, 0x48, 0x00, 0x01, 0x80,
        0x14, 0x00, 0x01, 0x80, 0x08, 0x00, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0x08, 0x00, 0x02, 0x80,
        0x04, 0x00, 0x02, 0x00, 0x1c, 0x00, 0x02, 0x80, 0x08, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00,
        0x10, 0x00, 0x02, 0x80, 0x04, 0x00, 0x03, 0x00, 0x04, 0x00, 0x08, 0x00, 0x04, 0x00, 0x09, 0x00,
        0x14, 0x00, 0x03, 0x80, 0x08, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x08, 0x00, 0x02, 0x80,
        0x04, 0x00, 0x0a, 0x00, 0x08, 0x00, 0x04, 0x00, 0x01, 0x00, 0x00, 0x00, 0x08, 0x00, 0x02, 0x00,
        0x04, 0x00, 0x00, 0x00, 0x54, 0x00, 0x03, 0x80, 0x40, 0x00, 0x01, 0x80, 0x14, 0x00, 0x01, 0x80,
        0x08, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x08, 0x00, 0x02, 0x80, 0x04, 0x00, 0x02, 0x00,
        0x14, 0x00, 0x02, 0x80, 0x08, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x08, 0x00, 0x02, 0x80,
        0x04, 0x00, 0x0c, 0x00, 0x14, 0x00, 0x03, 0x80, 0x08, 0x00, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00,
        0x08, 0x00, 0x02, 0x80, 0x04, 0x00, 0x0d, 0x00, 0x08, 0x00, 0x04, 0x00, 0x03, 0x00, 0x00, 0x00,
        0x08, 0x00, 0x02, 0x00, 0x04, 0x00, 0x00, 0x00,
    ];
    const WITHOUT_SECOND: &[u8] = &[
        0x58, 0x00, 0x01, 0x80, 0x44, 0x00, 0x01, 0x80, 0x14, 0x00, 0x01, 0x80, 0x08, 0x00, 0x01, 0x00,
        0x02, 0x00, 0x00, 0x00, 0x08, 0x00, 0x02, 0x80, 0x04, 0x00, 0x02, 0x00, 0x18, 0x00, 0x02, 0x80,
        0x08, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x0c, 0x00, 0x02, 0x80, 0x04, 0x00, 0x08, 0x00,
        0x04, 0x00, 0x09, 0x00, 0x14, 0x00, 0x03, 0x80, 0x08, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00,
        0x08, 0x00, 0x02, 0x80, 0x04, 0x00, 0x0a, 0x00, 0x08, 0x00, 0x04, 0x00, 0x02, 0x00, 0x00, 0x00,
        0x08, 0x00, 0x02, 0x00, 0x04, 0x00, 0x00, 0x00, 0x54, 0x00, 0x02, 0x80, 0x40, 0x00, 0x01, 0x80,
        0x14, 0x00, 0x01, 0x80, 0x08, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x08, 0x00, 0x02, 0x80,
        0x04, 0x00, 0x02, 0x00, 0x14, 0x00, 0x02, 0x80, 0x08, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00,
        0x08, 0x00, 0x02, 0x80, 0x04, 0x00, 0x0c, 0x00, 0x14, 0x00, 0x03, 0x80, 0x08, 0x00, 0x01, 0x00,
        0x02, 0x00, 0x00, 0x00, 0x08, 0x00, 0x02, 0x80, 0x04, 0x00, 0x0d, 0x00, 0x08, 0x00, 0x04, 0x00,
        0x03, 0x00, 0x00, 0x00, 0x08, 0x00, 0x02, 0x00, 0x04, 0x00, 0x00, 0x00,
    ];
    const SHARED_LIMIT_OF_ONE: &[u8] = &[
        0x30, 0x00, 0x01, 0x80, 0x1c, 0x00, 0x01, 0x80, 0x18, 0x00, 0x01, 0x80, 0x08, 0x00, 0x01, 0x00,
        0x01, 0x00, 0x00, 0x00, 0x0c, 0x00, 0x02, 0x80, 0x04, 0x00, 0x02, 0x00, 0x04, 0x00, 0x03, 0x00,
        0x08, 0x00, 0x04, 0x00, 0x01, 0x00, 0x00, 0x00, 0x08, 0x00, 0x02, 0x00, 0x02, 0x00, 0x00, 0x00,
    ];

    #[test]
    fn combination_parser_finds_ap_beside_station() {
        assert!(combinations_allow_ap_with_station(THIS_MACHINE));
        assert!(!combinations_allow_ap_with_station(WITHOUT_SECOND));
        assert!(!combinations_allow_ap_with_station(SHARED_LIMIT_OF_ONE));
    }

    // A GET_INTERFACE reply's attributes as `iw dev wlan0ap info` shows them: wiphy 0, ifindex 9, type AP.
    const AP_REPLY: &[u8] = &[
        0x08, 0x00, 0x03, 0x00, 0x09, 0x00, 0x00, 0x00, 0x0c, 0x00, 0x04, 0x00, b'w', b'l', b'a', b'n',
        b'0', b'a', b'p', 0x00, 0x08, 0x00, 0x05, 0x00, 0x03, 0x00, 0x00, 0x00, 0x08, 0x00, 0x01, 0x00,
        0x00, 0x00, 0x00, 0x00,
    ];

    #[test]
    fn only_an_ap_named_after_a_client_on_its_card_is_ours() {
        let ap = Iface { wiphy: 0, iftype: u32::from(NL80211_IFTYPE_AP) };
        assert_eq!(iface_of(&attrs(AP_REPLY)), Some(ap));
        let station = Iface { wiphy: 0, iftype: u32::from(NL80211_IFTYPE_STATION) };
        let wifi = [("wlan0".to_owned(), station), ("wlan0ap".to_owned(), ap)];
        assert!(is_ours("wlan0ap", ap, &wifi));
        assert!(!is_ours("wlan0ap", station, &wifi), "a station is never ours");
        assert!(!is_ours("wlan0ap", Iface { wiphy: 1, ..ap }, &wifi), "another card");
        assert!(!is_ours("wlan1ap", ap, &wifi), "no client wlan1");
        assert!(!is_ours("wlan0", ap, &wifi), "the client itself");
        let full = "abcdefghijklmap".to_owned();
        assert!(!is_ours(&full, ap, &[(full.clone(), ap)]), "a 15-char name is not its own client");
    }

    #[test]
    fn ap_name_fits_ifnamsiz() {
        assert_eq!(ap_name("wlan0"), "wlan0ap");
        assert_eq!(ap_name("wlp0s20f3abcdefgh").len(), IFNAME_MAX);
    }
}
