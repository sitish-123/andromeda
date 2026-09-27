//! Being findable: the mDNS service a sending phone connects to, and the BLE advertisement that makes an Android
//! phone look for it in the first place.

use std::collections::HashMap;
use std::net::SocketAddr;
use std::time::Duration;

use mdns_sd::{ServiceDaemon, ServiceEvent, ServiceInfo};
use ring::rand::{SecureRandom, SystemRandom};
use zbus::zvariant::{ObjectPath, OwnedValue, Value};

use crate::endpoint::{self, Endpoint, SERVICE_TYPE};
use crate::error::{Error, Result};

const BLE_SERVICE: &str = "fe2c";
const BLE_PATH: &str = "/org/umbriel/QuickShare/advertisement";
/// Android's "a Quick Share receiver is nearby" marker; the random tail makes each advertisement distinct.
const BLE_PREFIX: [u8; 5] = [0xFC, 0x12, 0x8E, 0x01, 0x42];

/// Registered while alive; dropping it sends the mDNS goodbye.
pub struct Mdns {
    daemon: ServiceDaemon,
    fullname: String,
}

impl Mdns {
    pub fn advertise(own: &Endpoint, port: u16) -> Result<Self> {
        let daemon = ServiceDaemon::new().map_err(mdns_error)?;
        let host = format!("{}.local.", own.id_str());
        let txt = own.txt_record()?;
        let properties = [("n", txt.as_str())];
        let info = ServiceInfo::new(SERVICE_TYPE, &own.instance_name(), &host, "", port, &properties[..])
            .map_err(mdns_error)?
            .enable_addr_auto();
        let fullname = info.get_fullname().to_owned();
        daemon.register(info).map_err(mdns_error)?;
        Ok(Self { daemon, fullname })
    }
}

impl Drop for Mdns {
    fn drop(&mut self) {
        if let Err(error) = self.daemon.unregister(&self.fullname) {
            log::debug!("mdns unregister: {error}");
        }
        drop(self.daemon.shutdown());
    }
}

pub struct Found {
    pub name: String,
    pub addresses: Vec<SocketAddr>,
}

/// Receivers seen within `window`, or only until one named `want` resolves with an IPv4 address.
pub async fn browse(window: Duration, want: Option<&str>) -> Result<Vec<Found>> {
    let daemon = ServiceDaemon::new().map_err(mdns_error)?;
    let events = daemon.browse(SERVICE_TYPE).map_err(mdns_error)?;
    let deadline = tokio::time::Instant::now() + window;
    let mut found: Vec<Found> = Vec::new();
    while let Ok(Ok(event)) = tokio::time::timeout_at(deadline, events.recv_async()).await {
        let ServiceEvent::ServiceResolved(service) = event else { continue };
        let Some(info) = service.get_property_val_str("n").and_then(decode_txt) else { continue };
        let Ok(peer) = endpoint::parse_info(&info) else { continue };
        let port = service.get_port();
        let addresses: Vec<SocketAddr> =
            service.get_addresses().iter().map(|ip| SocketAddr::new(ip.to_ip_addr(), port)).collect();
        let done = want == Some(peer.name.as_str()) && addresses.iter().any(SocketAddr::is_ipv4);
        // A service resolves again as more of its addresses arrive; keep the latest.
        found.retain(|known| known.name != peer.name);
        found.push(Found { name: peer.name, addresses });
        if done {
            break;
        }
    }
    drop(daemon.shutdown());
    Ok(found)
}

fn decode_txt(value: &str) -> Option<Vec<u8>> {
    use base64::Engine as _;
    base64::engine::general_purpose::URL_SAFE_NO_PAD.decode(value.trim_end_matches('=')).ok()
}

fn mdns_error<E: std::fmt::Display>(error: E) -> Error {
    Error::Io(std::io::Error::other(error.to_string()))
}

struct Advertisement {
    data: Vec<u8>,
}

// BlueZ reads these as D-Bus properties, which zbus requires to take `&self`.
#[allow(clippy::unused_self)]
#[zbus::interface(name = "org.bluez.LEAdvertisement1")]
impl Advertisement {
    fn release(&self) {
        log::info!("BlueZ released the Quick Share advertisement");
    }

    #[zbus(property, name = "Type")]
    fn kind(&self) -> String {
        "broadcast".to_owned()
    }

    #[zbus(property, name = "ServiceUUIDs")]
    fn service_uuids(&self) -> Vec<String> {
        vec![BLE_SERVICE.to_owned()]
    }

    #[zbus(property)]
    fn service_data(&self) -> HashMap<String, OwnedValue> {
        let value = Value::from(self.data.clone()).try_to_owned().ok();
        value.map(|v| HashMap::from([(BLE_SERVICE.to_owned(), v)])).unwrap_or_default()
    }
}

/// Registered with `BlueZ` while alive; the system bus connection going away withdraws it.
pub struct Ble {
    _bus: zbus::Connection,
}

impl Ble {
    pub async fn advertise(adapter: &str) -> Result<Self> {
        let mut data = BLE_PREFIX.to_vec();
        data.extend_from_slice(&[0; 12]);
        let mut tail = [0; 10];
        SystemRandom::new().fill(&mut tail).map_err(|_| Error::Random)?;
        data.extend_from_slice(&tail);

        let bus = zbus::Connection::system().await.map_err(bus_error)?;
        bus.object_server().at(BLE_PATH, Advertisement { data }).await.map_err(bus_error)?;
        let manager =
            zbus::Proxy::new(&bus, "org.bluez", adapter, "org.bluez.LEAdvertisingManager1").await.map_err(bus_error)?;
        let path = ObjectPath::try_from(BLE_PATH).map_err(bus_error)?;
        let options: HashMap<String, OwnedValue> = HashMap::new();
        manager.call::<_, _, ()>("RegisterAdvertisement", &(path, options)).await.map_err(bus_error)?;
        Ok(Self { _bus: bus })
    }
}

fn bus_error<E: std::fmt::Display>(error: E) -> Error {
    Error::Io(std::io::Error::other(error.to_string()))
}
