//! andromeda-fileops: file operations for every app on the session bus (`org.andromeda.FileOps1`), so a copy
//! survives a closed window and all windows show the same jobs (docs/architecture-v2.md §12b). D-Bus activated;
//! exits after 10 min with no running job, and the undo history goes with it.

mod archive;
mod info;
mod jobs;
mod ops;
mod storage;
mod tags;
mod trash;

use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use anyhow::Result;
use tokio::signal::unix::{SignalKind, signal};

const NAME: &str = "org.andromeda.FileOps1";
const IDLE_EXIT: Duration = Duration::from_secs(600);

#[tokio::main(flavor = "current_thread")]
async fn main() -> Result<()> {
    env_logger::Builder::from_env(env_logger::Env::default().default_filter_or("info")).init();
    let activity = Arc::new(Mutex::new((0usize, Some(Instant::now()))));
    let manager = jobs::Manager::new(activity.clone());
    let _conn = zbus::connection::Builder::session().serve_at(jobs::PATH, manager).name(NAME).build().await?;
    log::info!("serving {NAME}");

    let mut term = signal(SignalKind::terminate())?;
    let mut int = signal(SignalKind::interrupt())?;
    let mut tick = tokio::time::interval(Duration::from_secs(60));
    loop {
        tokio::select! {
            _ = term.recv() => return shutdown(&activity).await,
            _ = int.recv() => return shutdown(&activity).await,
            _ = tick.tick() => {
                let (running, last) = *activity.lock().unwrap_or_else(std::sync::PoisonError::into_inner);
                if running == 0 && last.is_some_and(|t| t.elapsed() >= IDLE_EXIT) {
                    log::info!("idle; exiting");
                    break;
                }
            }
        }
    }
    Ok(())
}

/// Stops running jobs at their next check and gives them 3 s; a copy stopped there leaves no partial file.
async fn shutdown(activity: &jobs::Activity) -> Result<()> {
    jobs::SHUTDOWN.store(true, std::sync::atomic::Ordering::Relaxed);
    for _ in 0..30 {
        if activity.lock().unwrap_or_else(std::sync::PoisonError::into_inner).0 == 0 {
            break;
        }
        tokio::time::sleep(Duration::from_millis(100)).await;
    }
    // Dropping the runtime would wait for blocking job threads without bound.
    std::process::exit(0)
}
