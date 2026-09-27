use std::fs::{self, DirBuilder};
use std::os::unix::fs::DirBuilderExt;
use std::path::PathBuf;

use anyhow::Context;

pub struct Paths {
    pub identity: PathBuf,
    pub devices: PathBuf,
}

impl Paths {
    /// `$STATE_DIRECTORY` under systemd, else `$XDG_STATE_HOME/umbriel-link`, else `~/.local/state/umbriel-link`.
    pub fn create() -> anyhow::Result<Self> {
        let dir = std::env::var_os("STATE_DIRECTORY")
            .map(PathBuf::from)
            .or_else(|| std::env::var_os("XDG_STATE_HOME").map(|state| PathBuf::from(state).join("umbriel-link")))
            .or_else(|| std::env::var_os("HOME").map(|home| PathBuf::from(home).join(".local/state/umbriel-link")))
            .context("no state directory: set XDG_STATE_HOME or HOME")?;
        if !dir.exists() {
            DirBuilder::new().recursive(true).mode(0o700).create(&dir)?;
        }
        fs::metadata(&dir).with_context(|| format!("state directory {}", dir.display()))?;
        Ok(Self { identity: dir.join("identity.pk8"), devices: dir.join("devices.json") })
    }
}
