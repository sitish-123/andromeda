//! Link over Bluetooth, desktop side: an RFCOMM profile registered with `BlueZ`, whose connections arrive as
//! descriptors for the listener to run like QUIC connections. See "Bluetooth" in `link/ARCHITECTURE.md`.

use std::collections::HashMap;
use std::os::fd::OwnedFd;

use tokio::sync::mpsc;
use zbus::zvariant::{OwnedObjectPath, OwnedValue, Value};

/// The Link service in SDP, which the phone looks up to find the RFCOMM channel.
pub const SERVICE_UUID: &str = "5c3b1e5a-7d2f-4a8e-9b61-3f0c2d4e8a17";
const PROFILE_PATH: &str = "/org/umbriel/Link1/Bluetooth";
const ADAPTER: &str = "/org/bluez/hci0";
/// E2E only: a Unix socket stands in for RFCOMM, so the stream path runs without radios.
const TEST_SOCKET: &str = "UMBRIEL_LINK_TEST_BLUETOOTH_SOCKET";
const TEST_ADDRESS: &str = "00:00:00:00:00:01";
/// Connections waiting for the listener; more are refused.
const BACKLOG: usize = 4;

/// Where Bluetooth connections come from. With no adapter, `address` is None and nothing arrives.
pub struct Bluetooth {
    pub address: Option<String>,
    pub incoming: mpsc::Receiver<OwnedFd>,
    /// The profile lives as long as this connection to the system bus.
    _bus: Option<zbus::Connection>,
}

impl Bluetooth {
    pub async fn start() -> Self {
        let (tx, incoming) = mpsc::channel(BACKLOG);
        if let Some(path) = std::env::var_os(TEST_SOCKET) {
            return match test_socket(path.as_ref(), tx) {
                Ok(()) => Self { address: Some(TEST_ADDRESS.to_owned()), incoming, _bus: None },
                Err(error) => {
                    log::error!("the test Bluetooth socket: {error}");
                    Self { address: None, incoming, _bus: None }
                }
            };
        }
        match register(tx).await {
            Ok((bus, address)) => {
                log::info!("Bluetooth: listening on {address}");
                Self { address: Some(address), incoming, _bus: Some(bus) }
            }
            Err(error) => {
                log::info!("Bluetooth unavailable: {error}");
                Self { address: None, incoming, _bus: None }
            }
        }
    }
}

async fn register(tx: mpsc::Sender<OwnedFd>) -> zbus::Result<(zbus::Connection, String)> {
    let bus = zbus::Connection::system().await?;
    let adapter = zbus::Proxy::new(&bus, "org.bluez", ADAPTER, "org.bluez.Adapter1").await?;
    let address: String = adapter.get_property("Address").await?;
    bus.object_server().at(PROFILE_PATH, Profile { incoming: tx }).await?;
    let manager = zbus::Proxy::new(&bus, "org.bluez", "/org/bluez", "org.bluez.ProfileManager1").await?;
    // Link's TLS authenticates both ends with the paired keys, so BlueZ is not asked to bond or to prompt.
    let options: HashMap<&str, Value<'_>> = HashMap::from([
        ("Name", Value::from("Umbriel Link")),
        ("Role", Value::from("server")),
        ("RequireAuthentication", Value::from(false)),
        ("RequireAuthorization", Value::from(false)),
        ("AutoConnect", Value::from(false)),
    ]);
    let path = zbus::zvariant::ObjectPath::try_from(PROFILE_PATH)?;
    manager.call::<_, _, ()>("RegisterProfile", &(path, SERVICE_UUID, options)).await?;
    Ok((bus, address.to_ascii_uppercase()))
}

struct Profile {
    incoming: mpsc::Sender<OwnedFd>,
}

// zbus hands methods owned arguments and needs `&self` on each, even where the body uses neither.
#[allow(clippy::unused_self, clippy::needless_pass_by_value)]
#[zbus::interface(name = "org.bluez.Profile1")]
impl Profile {
    fn release(&self) {
        log::info!("BlueZ released the Link profile");
    }

    fn new_connection(
        &self,
        device: OwnedObjectPath,
        fd: zbus::zvariant::OwnedFd,
        properties: HashMap<String, OwnedValue>,
    ) -> zbus::fdo::Result<()> {
        log::info!("Bluetooth connection from {}", device.as_str());
        log::debug!("its RFCOMM properties: {properties:?}");
        self.incoming
            .try_send(fd.into())
            .map_err(|_| zbus::fdo::Error::LimitsExceeded("too many Bluetooth connections waiting".to_owned()))
    }

    fn request_disconnection(&self, device: OwnedObjectPath) {
        log::info!("BlueZ asks to disconnect {}", device.as_str());
    }
}

fn test_socket(path: &std::path::Path, tx: mpsc::Sender<OwnedFd>) -> std::io::Result<()> {
    drop(std::fs::remove_file(path));
    let listener = tokio::net::UnixListener::bind(path)?;
    tokio::spawn(async move {
        while let Ok((stream, _)) = listener.accept().await {
            let Ok(stream) = stream.into_std() else { continue };
            if tx.send(stream.into()).await.is_err() {
                return;
            }
        }
    });
    Ok(())
}
