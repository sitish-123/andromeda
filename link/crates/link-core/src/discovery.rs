//! mDNS/DNS-SD: the desktop advertises `_umbriel-link._udp`, the phone browses for it.

use std::net::{IpAddr, SocketAddr};
use std::time::Duration;

use mdns_sd::{ServiceDaemon, ServiceEvent, ServiceInfo};

use crate::Error;
use crate::identity::DeviceId;

pub const SERVICE_TYPE: &str = "_umbriel-link._udp.local.";

/// A registered advertisement; dropping it sends the goodbye and stops the responder thread.
pub struct Advertiser {
    daemon: ServiceDaemon,
    id: DeviceId,
    port: u16,
    fullname: String,
}

#[derive(Debug, Clone)]
pub struct Found {
    pub id: DeviceId,
    pub pairing: bool,
    pub addresses: Vec<SocketAddr>,
}

impl Advertiser {
    pub fn start(id: &DeviceId, port: u16) -> Result<Self, Error> {
        let daemon = ServiceDaemon::new()?;
        let fullname = register(&daemon, id, port, false)?;
        Ok(Self { daemon, id: id.clone(), port, fullname })
    }

    /// Re-announces with `pair=1` while a pairing window is open.
    pub fn set_pairing(&self, open: bool) -> Result<(), Error> {
        register(&self.daemon, &self.id, self.port, open).map(|_| ())
    }
}

impl Drop for Advertiser {
    fn drop(&mut self) {
        if let Err(error) = self.daemon.unregister(&self.fullname) {
            log::debug!("mdns unregister: {error}");
        }
        if let Err(error) = self.daemon.shutdown() {
            log::debug!("mdns shutdown: {error}");
        }
    }
}

fn register(daemon: &ServiceDaemon, id: &DeviceId, port: u16, pairing: bool) -> Result<String, Error> {
    let mut properties = vec![("v", "1")];
    if pairing {
        properties.push(("pair", "1"));
    }
    let host = format!("{id}.local.");
    let info = ServiceInfo::new(SERVICE_TYPE, id.as_str(), &host, "", port, &properties[..])?.enable_addr_auto();
    let fullname = info.get_fullname().to_owned();
    daemon.register(info)?;
    Ok(fullname)
}

/// Browses for up to `window`, returning early once `want` resolves.
pub async fn browse(window: Duration, want: Option<&DeviceId>) -> Result<Vec<Found>, Error> {
    let daemon = ServiceDaemon::new()?;
    let events = daemon.browse(SERVICE_TYPE)?;
    let deadline = tokio::time::Instant::now() + window;
    let mut found: Vec<Found> = Vec::new();
    while let Ok(Ok(event)) = tokio::time::timeout_at(deadline, events.recv_async()).await {
        let ServiceEvent::ServiceResolved(service) = event else {
            continue;
        };
        let Some(desktop) = to_found(&service) else { continue };
        let done = want.is_some_and(|id| *id == desktop.id);
        found.retain(|known| known.id != desktop.id);
        found.push(desktop);
        if done {
            break;
        }
    }
    if let Err(error) = daemon.shutdown() {
        log::debug!("mdns shutdown: {error}");
    }
    Ok(found)
}

fn to_found(service: &mdns_sd::ResolvedService) -> Option<Found> {
    let instance = service.get_fullname().strip_suffix(SERVICE_TYPE)?.strip_suffix('.')?;
    let id = DeviceId::parse(instance).ok()?;
    let port = service.get_port();
    let addresses = service
        .get_addresses()
        .iter()
        .map(mdns_sd::ScopedIp::to_ip_addr)
        .filter(|ip| !matches!(ip, IpAddr::V6(v6) if v6.is_unicast_link_local()))
        .map(|ip| SocketAddr::new(ip, port))
        .collect();
    Some(Found { id, pairing: service.get_property_val_str("pair") == Some("1"), addresses })
}
