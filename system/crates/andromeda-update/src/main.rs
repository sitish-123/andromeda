//! andromeda-update: software update checks for the Settings page (docs/system.md §1, U1). Session bus,
//! `org.andromeda.Update1`, D-Bus activated; no root. Installs are the root helper's job (U2).

mod flatpak;
mod fwupd;
mod news;
mod pacman;
mod service;

use std::path::PathBuf;

use anyhow::{Context, Result};
use tokio::signal::unix::{SignalKind, signal};

const NAME: &str = "org.andromeda.Update1";

fn xdg(var: &str, fallback: &str) -> Result<PathBuf> {
    std::env::var_os(var)
        .map(PathBuf::from)
        .filter(|p| p.is_absolute())
        .or_else(|| std::env::var_os("HOME").map(|home| PathBuf::from(home).join(fallback)))
        .context("no HOME")
}

#[tokio::main(flavor = "current_thread")]
async fn main() -> Result<()> {
    env_logger::Builder::from_env(env_logger::Env::default().default_filter_or("info")).init();
    let cache = xdg("XDG_CACHE_HOME", ".cache")?.join("andromeda/update");
    let state = xdg("XDG_STATE_HOME", ".local/state")?.join("andromeda/update");
    let _conn = zbus::connection::Builder::session()
        .serve_at(service::PATH, service::Update::new(cache, state))
        .name(NAME)
        .build()
        .await?;
    log::info!("serving {NAME}");
    let mut term = signal(SignalKind::terminate())?;
    let mut int = signal(SignalKind::interrupt())?;
    tokio::select! {
        _ = term.recv() => {}
        _ = int.recv() => {}
    }
    Ok(())
}
