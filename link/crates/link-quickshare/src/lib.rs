//! Quick Share (Nearby Share) over the local network, written from the protocol as `NearDrop` documents it: mDNS and a
//! BLE hint for discovery, UKEY2 over TCP, an AES-CBC/HMAC secure channel, and the sharing frames on top.

pub mod advertise;
pub mod connection;
pub mod endpoint;
pub mod error;
pub mod frame;
pub mod receive;
pub mod secure;
pub mod send;
pub mod ukey2;
pub mod wire;

pub use error::{Error, Result};
