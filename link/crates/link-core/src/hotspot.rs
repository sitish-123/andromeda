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
