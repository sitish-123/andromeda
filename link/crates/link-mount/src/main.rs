//! `umbriel-link-mount`: connected phones' storage at `~/Phone` (or the path given), read-only, for phones whose
//! browse switch for this desktop is on. Outside `umbriel-linkd`'s sandbox, which can neither open `/dev/fuse` nor
//! mount where the user sees it; it reaches the daemon over D-Bus. See "Browsing" in `link/ARCHITECTURE.md`.

mod fs;
mod link;

use std::os::unix::fs::MetadataExt;
use std::path::{Path, PathBuf};

use anyhow::Context;
use fuser::{Config, MountOption};
use tokio::signal::unix::{SignalKind, signal};

/// `ENOTCONN`: a FUSE mount whose process is gone.
const STALE_MOUNT: i32 = 107;

fn main() -> anyhow::Result<()> {
    env_logger::Builder::from_env(env_logger::Env::default().default_filter_or("info,zbus=warn,tracing=warn"))
        .format_timestamp(None)
        .init();
    let mountpoint = match std::env::args_os().nth(1) {
        Some(path) => PathBuf::from(path),
        None => PathBuf::from(std::env::var_os("HOME").context("HOME is not set")?).join("Phone"),
    };
    prepare(&mountpoint)?;
    let owner = std::fs::metadata(&mountpoint)?;
    let runtime = tokio::runtime::Builder::new_multi_thread().worker_threads(1).enable_all().build()?;
    let link = runtime.block_on(link::Link::connect()).context("connecting to the session bus")?;
    let mut config = Config::default();
    config.mount_options = vec![
        MountOption::FSName("umbriel-link".to_owned()),
        MountOption::Subtype("umbriel-link".to_owned()),
        MountOption::RO,
        MountOption::NoExec,
        MountOption::NoSuid,
        MountOption::NoDev,
    ];
    // A slow read from one phone must not stall a listing of another.
    config.n_threads = Some(4);
    let filesystem = fs::PhoneFs::new(link, runtime.handle().clone(), owner.uid(), owner.gid());
    let session = fuser::spawn_mount(filesystem, &mountpoint, &config)
        .with_context(|| format!("mounting {}", mountpoint.display()))?;
    log::info!("phones at {}", mountpoint.display());
    runtime.block_on(async {
        let mut terminate = signal(SignalKind::terminate())?;
        tokio::select! {
            _ = terminate.recv() => {}
            _ = tokio::signal::ctrl_c() => {}
        }
        anyhow::Ok(())
    })?;
    session.umount_and_join().context("unmounting")
}

/// Creates the mount point, or clears a mount left by a process that died.
fn prepare(mountpoint: &Path) -> anyhow::Result<()> {
    match std::fs::metadata(mountpoint) {
        Err(error) if error.raw_os_error() == Some(STALE_MOUNT) => {
            log::info!("clearing a stale mount at {}", mountpoint.display());
            let status = std::process::Command::new("fusermount3").arg("-u").arg("-z").arg(mountpoint).status()?;
            anyhow::ensure!(status.success(), "fusermount3 could not clear {}", mountpoint.display());
            Ok(())
        }
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => Ok(std::fs::create_dir_all(mountpoint)?),
        Err(error) => Err(error.into()),
        Ok(_) => Ok(()),
    }
}
