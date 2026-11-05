//! The `network.wifi_power_save` actuator (docs/system-health.md §5): nl80211 power save on each Wi-Fi interface, as
//! `iw dev <if> set power_save` does. NetworkManager cannot reapply power save to a live connection.

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

    fn request(&self, cmd: u8, flags: u16, attributes: &[[u8; 8]]) -> Result<Vec<u8>> {
        let total = 20 + 8 * attributes.len();
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
        let (reply, _) = self.socket.recv_from_full().context("netlink recv")?;
        match error_code(&reply) {
            Some(0) | None => Ok(reply),
            Some(code) => Err(std::io::Error::from_raw_os_error(-code).into()),
        }
    }

    fn power_save(&self, ifindex: u32) -> Result<bool> {
        let reply = self.request(NL80211_CMD_GET_POWER_SAVE, 0, &[u32_attr(NL80211_ATTR_IFINDEX, ifindex)])?;
        let len = usize::try_from(u32::from_ne_bytes([reply[0], reply[1], reply[2], reply[3]]))?.min(reply.len());
        attrs(reply.get(20..len).unwrap_or_default())
            .into_iter()
            .find(|(kind, value)| *kind == NL80211_ATTR_PS_STATE && value.len() == 4)
            .map(|(_, value)| u32::from_ne_bytes([value[0], value[1], value[2], value[3]]) != 0)
            .ok_or_else(|| anyhow!("no power save state"))
    }

    fn set_power_save(&self, ifindex: u32, on: bool) -> Result<()> {
        let attributes = [u32_attr(NL80211_ATTR_IFINDEX, ifindex), u32_attr(NL80211_ATTR_PS_STATE, u32::from(on))];
        self.request(NL80211_CMD_SET_POWER_SAVE, NLM_F_ACK, &attributes).map(drop)
    }
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
