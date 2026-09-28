use std::net::{IpAddr, SocketAddr};

/// This machine's addresses another device could dial: not loopback, not IPv6 link-local (its scope is ours), not
/// a container or VM bridge. IPv4 before IPv6, and overlay networks (Tailscale's `100.64.0.0/10` and
/// `fd7a:115c:a1e0::/48`) right after the LAN, so a peer that keeps only the first few still reaches this machine from
/// anywhere.
pub fn local_addresses(port: u16) -> Vec<SocketAddr> {
    let interfaces = if_addrs::get_if_addrs().unwrap_or_else(|error| {
        log::warn!("listing interfaces: {error}");
        Vec::new()
    });
    let mut addresses: Vec<SocketAddr> = interfaces
        .iter()
        .filter(|interface| !interface.is_loopback() && !is_bridge(&interface.name))
        .map(if_addrs::Interface::ip)
        .filter(|ip| !matches!(ip, IpAddr::V6(v6) if v6.is_unicast_link_local()))
        .map(|ip| SocketAddr::new(ip, port))
        .collect();
    addresses.sort_by_key(|addr| rank(addr.ip()));
    addresses
}

/// Interfaces that only reach containers or VMs on this machine.
fn is_bridge(name: &str) -> bool {
    ["docker", "br-", "veth", "virbr", "podman", "cni", "vnet", "lxc"].iter().any(|prefix| name.starts_with(prefix))
}

/// LAN IPv4, overlay IPv4, LAN IPv6, overlay IPv6.
fn rank(ip: IpAddr) -> u8 {
    match ip {
        IpAddr::V4(v4) => u8::from(v4.octets()[0] == 100 && (v4.octets()[1] & 0xc0) == 64),
        IpAddr::V6(v6) => 2 + u8::from(v6.segments()[..3] == [0xfd7a, 0x115c, 0xa1e0]),
    }
}
