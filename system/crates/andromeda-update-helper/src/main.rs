//! andromeda-update-helper: root actions for Software Update (docs/system.md §1, U2). System bus,
//! `org.andromeda.UpdateHelper1`, D-Bus activated through systemd; exits after 5 idle minutes.

mod helper;
mod migrations;
mod snapshot;
mod transaction;

use std::sync::Arc;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::time::{Duration, Instant};

use anyhow::Result;

const NAME: &str = "org.andromeda.UpdateHelper1";
const IDLE_EXIT: Duration = Duration::from_secs(300);

#[tokio::main(flavor = "current_thread")]
async fn main() -> Result<()> {
    env_logger::Builder::from_env(env_logger::Env::default().default_filter_or("info")).init();
    // systemd sets STATE_DIRECTORY from StateDirectory=.
    let state_dir: std::path::PathBuf =
        std::env::var_os("STATE_DIRECTORY").map_or_else(|| "/var/lib/andromeda-update".into(), Into::into);
    // The package's install script and pacman hook.
    let migrations_dir = std::path::Path::new(migrations::DIR);
    match std::env::args().nth(1).as_deref() {
        Some("--seed-migrations") => return migrations::seed(migrations_dir, &state_dir, true),
        Some("--run-migrations") => return migrations::run(migrations_dir, &state_dir, true).map(|_| ()),
        _ => {}
    }
    let running = Arc::new(AtomicBool::new(false));
    let last_call = Arc::new(AtomicU64::new(0));
    let started = Instant::now();
    let helper = helper::Helper { state_dir, running: running.clone(), last_call: last_call.clone(), started };
    let _conn = zbus::connection::Builder::system()
        .serve_at(helper::PATH, helper)
        .name(NAME)
        .build()
        .await?;
    log::info!("serving {NAME}");
    let mut tick = tokio::time::interval(Duration::from_secs(30));
    loop {
        tick.tick().await;
        let since_call = started.elapsed().saturating_sub(Duration::from_secs(last_call.load(Ordering::Relaxed)));
        if !running.load(Ordering::Relaxed) && since_call > IDLE_EXIT {
            return Ok(());
        }
    }
}
