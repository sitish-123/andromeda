use std::net::{IpAddr, SocketAddr};

/// This machine's addresses another device could dial: not loopback, not IPv6 link-local (its scope is ours).
pub fn local_addresses(port: u16) -> Vec<SocketAddr> {
    let interfaces = if_addrs::get_if_addrs().unwrap_or_else(|error| {
        log::warn!("listing interfaces: {error}");
        Vec::new()
    });
    interfaces
        .iter()
        .filter(|interface| !interface.is_loopback())
        .map(if_addrs::Interface::ip)
        .filter(|ip| !matches!(ip, IpAddr::V6(v6) if v6.is_unicast_link_local()))
        .map(|ip| SocketAddr::new(ip, port))
        .collect()
}
