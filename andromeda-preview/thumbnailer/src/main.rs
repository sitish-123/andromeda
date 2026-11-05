//! D-Bus-activated thumbnail service: org.freedesktop.thumbnails.Thumbnailer1 on the session bus. Requests run
//! one at a time in background.slice; the process exits after a minute with nothing to do.

use andromeda_thumbnailer::cache::{self, Flavor};
use andromeda_thumbnailer::registry::Registry;
use andromeda_thumbnailer::runner::{Error, Runner, Sandbox};
use std::collections::HashSet;
use std::sync::{Arc, Mutex};
use std::time::Duration;
use tokio::sync::mpsc;
use zbus::object_server::SignalEmitter;

const BUS_NAME: &str = "org.freedesktop.thumbnails.Thumbnailer1";
const PATH: &str = "/org/freedesktop/thumbnails/Thumbnailer1";
const IDLE_EXIT: Duration = Duration::from_secs(60);
const THUMBNAILER_TIMEOUT: Duration = Duration::from_secs(30);
/// Error codes in `Error`: tumbler sends GIO error codes (G_IO_ERROR_FAILED, G_IO_ERROR_NOT_SUPPORTED).
const ERROR_FAILED: i32 = 0;
const ERROR_UNSUPPORTED: i32 = 15;

struct Job {
    handle: u32,
    uris: Vec<String>,
    mimes: Vec<String>,
    flavor: Flavor,
}

struct Service {
    next: Mutex<u32>,
    jobs: mpsc::UnboundedSender<Job>,
    dequeued: Arc<Mutex<HashSet<u32>>>,
    mime_types: Vec<String>,
}

#[zbus::interface(name = "org.freedesktop.thumbnails.Thumbnailer1")]
impl Service {
    async fn queue(
        &self,
        uris: Vec<String>,
        mime_types: Vec<String>,
        flavor: String,
        _scheduler: String,
        handle_to_dequeue: u32,
    ) -> zbus::fdo::Result<u32> {
        let flavor = Flavor::parse(&flavor).ok_or_else(|| zbus::fdo::Error::InvalidArgs("unknown flavor".into()))?;
        if uris.len() != mime_types.len() {
            return Err(zbus::fdo::Error::InvalidArgs("one MIME type per URI".into()));
        }
        if handle_to_dequeue != 0 {
            self.dequeued.lock().unwrap().insert(handle_to_dequeue);
        }
        let handle = {
            let mut next = self.next.lock().unwrap();
            *next = next.wrapping_add(1).max(1);
            *next
        };
        let _ = self.jobs.send(Job { handle, uris, mimes: mime_types, flavor });
        Ok(handle)
    }

    async fn dequeue(&self, handle: u32) {
        self.dequeued.lock().unwrap().insert(handle);
    }

    async fn get_supported(&self) -> (Vec<String>, Vec<String>) {
        (vec!["file".to_owned(); self.mime_types.len()], self.mime_types.clone())
    }

    async fn get_schedulers(&self) -> Vec<String> {
        vec!["default".to_owned(), "foreground".to_owned(), "background".to_owned()]
    }

    async fn get_flavors(&self) -> Vec<String> {
        Flavor::ALL.iter().map(|f| f.dir().to_owned()).collect()
    }

    #[zbus(signal)]
    async fn started(emitter: &SignalEmitter<'_>, handle: u32) -> zbus::Result<()>;
    #[zbus(signal)]
    async fn finished(emitter: &SignalEmitter<'_>, handle: u32) -> zbus::Result<()>;
    #[zbus(signal)]
    async fn ready(emitter: &SignalEmitter<'_>, handle: u32, uris: Vec<String>) -> zbus::Result<()>;
    #[zbus(signal)]
    async fn error(
        emitter: &SignalEmitter<'_>,
        handle: u32,
        failed_uris: Vec<String>,
        error_code: i32,
        message: String,
    ) -> zbus::Result<()>;
}

async fn run_job(
    emitter: &SignalEmitter<'static>,
    runner: Option<&Arc<Runner>>,
    job: Job,
    dequeued: &Mutex<HashSet<u32>>,
) {
    if dequeued.lock().unwrap().remove(&job.handle) {
        return;
    }
    let _ = Service::started(emitter, job.handle).await;
    for (uri, mime) in job.uris.into_iter().zip(job.mimes) {
        if dequeued.lock().unwrap().contains(&job.handle) {
            break;
        }
        let Some(r) = runner.map(Arc::clone) else {
            let _ = Service::error(emitter, job.handle, vec![uri], ERROR_FAILED, "sandbox unavailable".into()).await;
            continue;
        };
        let (uri_c, flavor) = (uri.clone(), job.flavor);
        let result = tokio::task::spawn_blocking(move || r.thumbnail(&uri_c, &mime, flavor)).await;
        let _ = match result {
            Ok(Ok(_)) => Service::ready(emitter, job.handle, vec![uri]).await,
            Ok(Err(Error::Unsupported | Error::NotLocal)) => {
                Service::error(emitter, job.handle, vec![uri], ERROR_UNSUPPORTED, "unsupported".into()).await
            }
            Ok(Err(e)) => Service::error(emitter, job.handle, vec![uri], ERROR_FAILED, format!("{e:?}")).await,
            Err(_) => Service::error(emitter, job.handle, vec![uri], ERROR_FAILED, "panic".into()).await,
        };
    }
    // Handles only grow: a dequeue of this or an older handle can no longer match a job.
    dequeued.lock().unwrap().retain(|&h| h > job.handle);
    let _ = Service::finished(emitter, job.handle).await;
}

async fn work(
    connection: &zbus::Connection,
    emitter: SignalEmitter<'static>,
    runner: Option<Arc<Runner>>,
    mut jobs: mpsc::UnboundedReceiver<Job>,
    dequeued: Arc<Mutex<HashSet<u32>>>,
) {
    loop {
        match tokio::time::timeout(IDLE_EXIT, jobs.recv()).await {
            Ok(Some(job)) => run_job(&emitter, runner.as_ref(), job, &dequeued).await,
            Ok(None) => return,
            Err(_) => break,
        }
    }
    // Leave the bus first, then finish what arrived meanwhile: a queued caller always gets Finished.
    let _ = connection.release_name(BUS_NAME).await;
    // No more Queue calls after this: a call already on the connection is answered with an error, not a handle.
    let _ = connection.object_server().remove::<Service, _>(PATH).await;
    while let Ok(job) = jobs.try_recv() {
        run_job(&emitter, runner.as_ref(), job, &dequeued).await;
    }
    log::info!("idle; exiting");
}

#[tokio::main(flavor = "current_thread")]
async fn main() -> zbus::Result<()> {
    env_logger::Builder::from_env(env_logger::Env::default().default_filter_or("info")).init();
    let root = cache::default_root().ok_or_else(|| zbus::Error::Failure("no HOME".into()))?;
    let sandboxed = std::env::var_os("ANDROMEDA_THUMBNAILER_NO_SANDBOX").is_none();
    let registry = Registry::load(&Registry::system_dirs(), sandboxed);
    let mut mime_types: Vec<String> = registry.mime_types().map(str::to_owned).collect();
    mime_types.sort();
    let sandbox = if sandboxed { Sandbox::Bwrap } else { Sandbox::None };
    let runner = Runner { root, registry, sandbox, timeout: THUMBNAILER_TIMEOUT };
    // Without a working sandbox every run would fail for a reason that is not the file's: answer with errors and
    // write no fail markers.
    let runner = if runner.sandbox_works() {
        Some(Arc::new(runner))
    } else {
        log::error!("thumbnailer sandbox (systemd-run + bwrap) does not start; no thumbnails");
        None
    };
    let (tx, rx) = mpsc::unbounded_channel();
    let dequeued = Arc::new(Mutex::new(HashSet::new()));
    let service = Service { next: Mutex::new(0), jobs: tx, dequeued: Arc::clone(&dequeued), mime_types };
    let connection =
        zbus::connection::Builder::session().name(BUS_NAME).serve_at(PATH, service).build().await?;
    let iface = connection.object_server().interface::<_, Service>(PATH).await?;
    work(&connection, iface.signal_emitter().clone(), runner, rx, dequeued).await;
    Ok(())
}
