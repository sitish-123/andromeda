//! The phone's hotspot, for moving a Bluetooth session to full speed. See "Hotspot" in `link/ARCHITECTURE.md`.

use std::time::Duration;

use link_proto::message::Hotspot;

/// A hotspot nothing moved through for this long is stopped.
pub const IDLE: Duration = Duration::from_secs(60);
/// How long a send over the size limit waits for the session to move to the hotspot.
pub const UPGRADE_TIMEOUT: Duration = Duration::from_secs(60);

/// Starts and stops a local-only hotspot: no tethering, no data use. Blocking; the phone calls it off the runtime.
pub trait HotspotProvider: Send + Sync {
    fn start(&self) -> std::io::Result<Hotspot>;
    fn stop(&self);
}

/// How long a Wi-Fi Direct group may take to form before the phone falls back to its hotspot.
pub const WIFI_DIRECT_TIMEOUT: Duration = Duration::from_secs(25);

/// Forms a Wi-Fi Direct group with a desktop, so the desktop keeps its own network; either side may end up owning it.
/// Blocking, like [`HotspotProvider`].
pub trait WifiDirectProvider: Send + Sync {
    /// The name other devices see while this phone looks for peers.
    fn name(&self) -> std::io::Result<String>;
    /// Finds `peer`, a device address or else a name, and forms the group; returns once it is up.
    fn connect(&self, peer: &str) -> std::io::Result<()>;
    fn stop(&self);
}
