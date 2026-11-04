//! andromeda-system: the desktop's one system API, `org.andromeda.System1` (docs/system-health.md). Runs on the
//! system bus; `DBUS_SYSTEM_BUS_ADDRESS` points it at a test bus.

mod battery;
mod bus;
mod engine;
mod explain;
mod expr;
mod holds;
mod kbd_backlight;
mod knobs;
mod manager;
mod memory;
mod network;
mod nm;
mod policy;
mod polkit;
mod power;
mod rules;
mod session;
mod state;
mod storage;
mod thermal;

use std::path::PathBuf;

use anyhow::Result;
use tokio::signal::unix::{SignalKind, signal};

pub const NAME: &str = "org.andromeda.System1";

#[tokio::main(flavor = "current_thread")]
async fn main() -> Result<()> {
    env_logger::Builder::from_env(env_logger::Env::default().default_filter_or("info")).init();
    if std::env::args().nth(1).as_deref() == Some("--session") {
        return session::run().await;
    }
    // systemd sets STATE_DIRECTORY from StateDirectory=; tests point it at a temp dir.
    let state = std::env::var_os("STATE_DIRECTORY").map_or_else(|| "/var/lib/andromeda-system".into(), PathBuf::from);
    let policy = policy::new(state.join("explain.jsonl"));
    let conn = zbus::connection::Builder::system()
        .serve_at(manager::PATH, zbus::fdo::ObjectManager)
        .serve_at(manager::PATH, manager::Manager::default())
        .serve_at(power::PATH, power::Power::default())
        .serve_at(memory::PATH, memory::Memory::default())
        .serve_at(network::PATH, network::Network::default())
        .serve_at(storage::PATH, storage::Storage::default())
        .serve_at(thermal::PATH, thermal::Thermal::default())
        .serve_at(knobs::PATH, knobs::Knobs { policy: policy.clone() })
        .serve_at(explain::PATH, explain::Explain { policy: policy.clone() })
        .name(NAME)
        .build()
        .await?;
    tokio::spawn(knobs::release_on_exit(conn.clone(), policy));
    tokio::spawn(power::run(conn.clone()));
    tokio::spawn(battery::run(conn.clone()));
    tokio::spawn(kbd_backlight::run(conn.clone()));
    tokio::spawn(power::sample_rapl(conn.clone()));
    tokio::spawn(memory::run(conn.clone()));
    tokio::spawn(network::run(conn.clone()));
    tokio::spawn(storage::run(conn.clone()));
    tokio::spawn(thermal::run(conn.clone()));
    let _ = sd_notify::notify(&[sd_notify::NotifyState::Ready]);
    if let Some(timeout) = sd_notify::watchdog_enabled() {
        // The ping runs on the same single-threaded loop as every backend, so a stuck loop misses it.
        tokio::spawn(async move {
            let mut tick = tokio::time::interval(timeout / 2);
            loop {
                tick.tick().await;
                let _ = sd_notify::notify(&[sd_notify::NotifyState::Watchdog]);
            }
        });
    }
    log::info!("serving {NAME}");

    let mut term = signal(SignalKind::terminate())?;
    let mut int = signal(SignalKind::interrupt())?;
    tokio::select! {
        _ = term.recv() => {}
        _ = int.recv() => {}
    }
    Ok(())
}
