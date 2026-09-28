use std::fs::{self, DirBuilder};
use std::os::unix::fs::DirBuilderExt;
use std::path::{Path, PathBuf};

use anyhow::Context;

pub struct Paths {
    pub state: PathBuf,
    pub identity: PathBuf,
    pub devices: PathBuf,
    pub downloads: PathBuf,
    /// Artwork of phone players, for their MPRIS `mpris:artUrl`; created on first use.
    pub art: PathBuf,
}

impl Paths {
    /// `$STATE_DIRECTORY` under systemd, else `$XDG_STATE_HOME/umbriel-link`, else `~/.local/state/umbriel-link`.
    pub fn create() -> anyhow::Result<Self> {
        let home = std::env::var_os("HOME").map(PathBuf::from);
        let dir = std::env::var_os("STATE_DIRECTORY")
            .map(PathBuf::from)
            .or_else(|| std::env::var_os("XDG_STATE_HOME").map(|state| PathBuf::from(state).join("umbriel-link")))
            .or_else(|| home.as_ref().map(|home| home.join(".local/state/umbriel-link")))
            .context("no state directory: set XDG_STATE_HOME or HOME")?;
        if !dir.exists() {
            DirBuilder::new().recursive(true).mode(0o700).create(&dir)?;
        }
        fs::metadata(&dir).with_context(|| format!("state directory {}", dir.display()))?;
        let downloads = download_dir(home.as_deref()).context("no download directory: set HOME")?;
        Ok(Self {
            identity: dir.join("identity.pk8"),
            devices: dir.join("devices.json"),
            art: dir.join("art"),
            state: dir,
            downloads,
        })
    }
}

/// `$XDG_DOWNLOAD_DIR`, else the one in `user-dirs.dirs`, else `~/Downloads`.
fn download_dir(home: Option<&Path>) -> Option<PathBuf> {
    if let Some(dir) = std::env::var_os("XDG_DOWNLOAD_DIR") {
        return Some(PathBuf::from(dir));
    }
    let home = home?;
    let config = std::env::var_os("XDG_CONFIG_HOME").map_or_else(|| home.join(".config"), PathBuf::from);
    let listed = fs::read_to_string(config.join("user-dirs.dirs")).ok().and_then(|dirs| {
        let line = dirs.lines().find_map(|line| line.trim().strip_prefix("XDG_DOWNLOAD_DIR="))?;
        let value = line.trim_matches('"');
        Some(match value.strip_prefix("$HOME") {
            Some(rest) => home.join(rest.trim_start_matches('/')),
            None => PathBuf::from(value),
        })
    });
    Some(listed.unwrap_or_else(|| home.join("Downloads")))
}
