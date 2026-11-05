//! `/org/andromeda/System1/Display`: keeps the last frame on screen between two compositors (login, logout).
//! The kernel restores the text console when the last open file of a DRM device closes. A compositor that exits
//! hands its DRM files here with `HoldScanout`; the next compositor calls `ReleaseScanout` after its first frame.

use std::os::fd::OwnedFd;
use std::sync::{Arc, Mutex};
use std::time::Duration;

use rustix::fs::{FileType, fstat, major, minor};
use zbus::message::Header;
use zbus::{Connection, interface};

use crate::polkit;

pub const PATH: &str = "/org/andromeda/System1/Display";
const DRM_MAJOR: u32 = 226;
// Card nodes are minors 0-63; render (128+) and control nodes never hold a scanout.
const CARD_MINORS: u32 = 64;
const MAX_HELD: usize = 8;
const HOLD_LIMIT: Duration = Duration::from_secs(15);

#[derive(Default)]
struct Held {
    files: Vec<OwnedFd>,
    generation: u64,
}

#[derive(Clone, Default)]
pub struct Display {
    held: Arc<Mutex<Held>>,
}

fn is_drm_card(fd: &OwnedFd) -> bool {
    fstat(fd).is_ok_and(|st| {
        FileType::from_raw_mode(st.st_mode) == FileType::CharacterDevice
            && major(st.st_rdev) == DRM_MAJOR
            && minor(st.st_rdev) < CARD_MINORS
    })
}

impl Display {
    fn release(&self) -> usize {
        let mut held = self.held.lock().unwrap_or_else(std::sync::PoisonError::into_inner);
        held.generation += 1;
        std::mem::take(&mut held.files).len()
    }
}

#[interface(name = "org.andromeda.System1.Display")]
impl Display {
    /// Keeps `fd`, an open DRM card node, until `ReleaseScanout` or 15 s. The caller already holds the file, so this
    /// grants nothing new; only a caller in an active session (greeter or user compositor) or root may hold.
    async fn hold_scanout(
        &self,
        fd: zbus::OwnedFd,
        #[zbus(header)] header: Header<'_>,
        #[zbus(connection)] conn: &Connection,
    ) -> zbus::fdo::Result<()> {
        polkit::check(conn, &header, "org.andromeda.system.hold-scanout").await?;
        let fd: OwnedFd = fd.into();
        if !is_drm_card(&fd) {
            return Err(zbus::fdo::Error::InvalidArgs("not a DRM card node".into()));
        }
        let generation = {
            let mut held = self.held.lock().unwrap_or_else(std::sync::PoisonError::into_inner);
            if held.files.len() >= MAX_HELD {
                return Err(zbus::fdo::Error::LimitsExceeded(format!("{MAX_HELD} files held")));
            }
            held.files.push(fd);
            held.generation += 1;
            held.generation
        };
        log::info!("display: holding scanout for the next compositor");
        let held = self.held.clone();
        tokio::spawn(async move {
            tokio::time::sleep(HOLD_LIMIT).await;
            let mut held = held.lock().unwrap_or_else(std::sync::PoisonError::into_inner);
            if held.generation == generation && !held.files.is_empty() {
                log::warn!("display: no compositor took over in {}s; console restored", HOLD_LIMIT.as_secs());
                held.files.clear();
            }
        });
        Ok(())
    }

    /// Drops every held file: the caller's first frame is on screen.
    async fn release_scanout(
        &self,
        #[zbus(header)] header: Header<'_>,
        #[zbus(connection)] conn: &Connection,
    ) -> zbus::fdo::Result<()> {
        polkit::check(conn, &header, "org.andromeda.system.hold-scanout").await?;
        if self.release() > 0 {
            log::info!("display: scanout released");
        }
        Ok(())
    }
}

