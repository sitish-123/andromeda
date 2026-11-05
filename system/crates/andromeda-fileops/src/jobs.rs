//! `org.andromeda.FileOps1`: one shared job list for every app. Each job runs on its own blocking thread and is an
//! object `/org/andromeda/FileOps1/Job/<n>` with live properties, so any window shows and answers it.

use std::ffi::OsString;
use std::os::unix::ffi::{OsStrExt, OsStringExt};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, AtomicU32, Ordering};
use std::sync::mpsc;
use std::sync::{Arc, Mutex, MutexGuard, PoisonError};
use std::time::{Duration, Instant};

use tokio::sync::Notify;
use zbus::object_server::InterfaceRef;
use zbus::{Connection, ObjectPath, OwnedObjectPath, interface};

use crate::ops::{self, Control, OpError, Resolution, UndoStep};
use crate::trash;

pub const PATH: &str = "/org/andromeda/FileOps1";
const EMIT_INTERVAL: Duration = Duration::from_millis(100);
const ANSWER_POLL: Duration = Duration::from_millis(250);

/// Set on SIGTERM: every job stops at its next check, also one that waits for a conflict answer.
pub static SHUTDOWN: AtomicBool = AtomicBool::new(false);

#[derive(Clone, Copy, PartialEq, Eq)]
enum State {
    Running,
    Conflict,
    Done,
    Failed,
    Cancelled,
}

impl State {
    fn name(self) -> &'static str {
        match self {
            Self::Running => "running",
            Self::Conflict => "conflict",
            Self::Done => "done",
            Self::Failed => "failed",
            Self::Cancelled => "cancelled",
        }
    }
}

struct Status {
    state: State,
    done_files: u64,
    done_bytes: u64,
    current: PathBuf,
    error: String,
    conflict: Option<(PathBuf, PathBuf)>,
    undo: Option<Vec<UndoStep>>,
}

type Answer = Option<(Resolution, bool)>;

struct Shared {
    kind: &'static str,
    total_files: u64,
    total_bytes: u64,
    status: Mutex<Status>,
    cancel: AtomicBool,
    answer: Mutex<Option<mpsc::Sender<Answer>>>,
    changed: Notify,
}

fn lock<T>(m: &Mutex<T>) -> MutexGuard<'_, T> {
    m.lock().unwrap_or_else(PoisonError::into_inner)
}

impl Control for Shared {
    fn cancelled(&self) -> bool {
        self.cancel.load(Ordering::Relaxed) || SHUTDOWN.load(Ordering::Relaxed)
    }

    fn progress(&self, files: u64, bytes: u64, current: &Path) {
        let mut status = lock(&self.status);
        status.done_files = files;
        status.done_bytes = bytes;
        current.clone_into(&mut status.current);
        drop(status);
        self.changed.notify_one();
    }

    fn conflict(&self, source: &Path, target: &Path) -> Option<(Resolution, bool)> {
        let (tx, rx) = mpsc::channel();
        *lock(&self.answer) = Some(tx);
        {
            let mut status = lock(&self.status);
            status.state = State::Conflict;
            status.conflict = Some((source.to_path_buf(), target.to_path_buf()));
        }
        self.changed.notify_one();
        // Any app may answer later (the job list is shared), so there is no deadline; cancel and shutdown end it.
        let answer = loop {
            if self.cancelled() {
                break None;
            }
            match rx.recv_timeout(ANSWER_POLL) {
                Ok(answer) => break answer,
                Err(mpsc::RecvTimeoutError::Timeout) => {}
                Err(mpsc::RecvTimeoutError::Disconnected) => break None,
            }
        };
        lock(&self.answer).take();
        let mut status = lock(&self.status);
        status.state = State::Running;
        status.conflict = None;
        drop(status);
        self.changed.notify_one();
        answer
    }
}

pub struct Job {
    shared: Arc<Shared>,
}

fn bytes(path: &Path) -> Vec<u8> {
    path.as_os_str().as_bytes().to_vec()
}

#[interface(name = "org.andromeda.FileOps1.Job")]
impl Job {
    /// copy, move, trash, delete, restore or undo.
    #[zbus(property(emits_changed_signal = "const"))]
    fn kind(&self) -> &str {
        self.shared.kind
    }
    /// running, conflict (waits for `ResolveConflict`), done, failed or cancelled.
    #[zbus(property)]
    fn state(&self) -> &str {
        lock(&self.shared.status).state.name()
    }
    #[zbus(property(emits_changed_signal = "const"))]
    fn total_files(&self) -> u64 {
        self.shared.total_files
    }
    #[zbus(property(emits_changed_signal = "const"))]
    fn total_bytes(&self) -> u64 {
        self.shared.total_bytes
    }
    #[zbus(property)]
    fn done_files(&self) -> u64 {
        lock(&self.shared.status).done_files
    }
    #[zbus(property)]
    fn done_bytes(&self) -> u64 {
        lock(&self.shared.status).done_bytes
    }
    #[zbus(property)]
    fn current(&self) -> Vec<u8> {
        bytes(&lock(&self.shared.status).current)
    }
    #[zbus(property)]
    fn error(&self) -> String {
        lock(&self.shared.status).error.clone()
    }
    /// (source, existing target) while State is conflict, else two empty paths.
    #[zbus(property)]
    fn conflict(&self) -> (Vec<u8>, Vec<u8>) {
        lock(&self.shared.status).conflict.as_ref().map(|(s, t)| (bytes(s), bytes(t))).unwrap_or_default()
    }
    #[zbus(property)]
    fn undoable(&self) -> bool {
        lock(&self.shared.status).undo.as_ref().is_some_and(|steps| !steps.is_empty())
    }

    fn cancel(&self) {
        self.shared.cancel.store(true, Ordering::Relaxed);
        if let Some(tx) = lock(&self.shared.answer).take() {
            let _ = tx.send(None);
        }
    }

    /// `resolution`: replace (folders merge; a replaced file goes to the trash), skip or `keep_both`.
    fn resolve_conflict(&self, resolution: &str, apply_to_all: bool) -> zbus::fdo::Result<()> {
        let resolution = Resolution::parse(resolution)
            .ok_or_else(|| zbus::fdo::Error::InvalidArgs(format!("unknown resolution '{resolution}'")))?;
        let tx = lock(&self.shared.answer).take().ok_or_else(|| zbus::fdo::Error::Failed("no conflict waits".into()))?;
        let _ = tx.send(Some((resolution, apply_to_all)));
        Ok(())
    }
}

/// (running jobs, when the last one finished)
pub type Activity = Arc<Mutex<(usize, Option<Instant>)>>;

pub struct Manager {
    jobs: Vec<(OwnedObjectPath, Arc<Shared>)>,
    next: AtomicU32,
    activity: Activity,
}

enum Request {
    Copy(Vec<PathBuf>, PathBuf),
    Move(Vec<PathBuf>, PathBuf),
    Trash(Vec<PathBuf>),
    Delete(Vec<PathBuf>),
    /// Steps, and the job they came from: steps an undo did not finish go back to it for a retry.
    Undo(Vec<UndoStep>, Option<Arc<Shared>>),
}

fn paths(raw: Vec<Vec<u8>>) -> zbus::fdo::Result<Vec<PathBuf>> {
    raw.into_iter()
        .map(|mut p| {
            // GLib bytestrings (`b'...'`, g_variant_new_bytestring) end in one NUL.
            if p.last() == Some(&0) {
                p.pop();
            }
            if p.contains(&0) {
                return Err(zbus::fdo::Error::InvalidArgs("NUL inside a path".into()));
            }
            let path = PathBuf::from(OsString::from_vec(p));
            if path.is_absolute() { Ok(path) } else { Err(zbus::fdo::Error::InvalidArgs("paths must be absolute".into())) }
        })
        .collect()
}

fn run(request: Request, shared: &Shared) {
    let (undo, result) = match request {
        Request::Copy(sources, dir) => ops::copy(&sources, &dir, shared),
        Request::Move(sources, dir) => ops::move_to(&sources, &dir, shared),
        Request::Trash(sources) => ops::trash_all(&sources, shared),
        Request::Delete(sources) => (Vec::new(), ops::delete(&sources, shared)),
        Request::Undo(steps, origin) => {
            let (redo, result, left) = ops::undo(&steps, shared);
            if let Some(origin) = origin.filter(|_| !left.is_empty()) {
                lock(&origin.status).undo = Some(left);
            }
            (redo, result)
        }
    };
    let mut status = lock(&shared.status);
    status.undo = Some(undo);
    match result {
        Ok(()) => status.state = State::Done,
        Err(OpError::Cancelled) => status.state = State::Cancelled,
        Err(e) => {
            status.state = State::Failed;
            status.error = e.to_string();
        }
    }
    drop(status);
    shared.changed.notify_one();
}

async fn publish(conn: Connection, path: OwnedObjectPath, shared: Arc<Shared>, activity: Activity) {
    let iface: zbus::Result<InterfaceRef<Job>> = conn.object_server().interface(&path).await;
    let Ok(iface) = iface else { return };
    loop {
        shared.changed.notified().await;
        let job = iface.get().await;
        let emitter = iface.signal_emitter();
        let _ = job.state_changed(emitter).await;
        let _ = job.done_files_changed(emitter).await;
        let _ = job.done_bytes_changed(emitter).await;
        let _ = job.current_changed(emitter).await;
        let _ = job.conflict_changed(emitter).await;
        let finished = matches!(lock(&shared.status).state, State::Done | State::Failed | State::Cancelled);
        if finished {
            let _ = job.error_changed(emitter).await;
            let _ = job.undoable_changed(emitter).await;
            let mut activity = lock(&activity);
            activity.0 -= 1;
            activity.1 = Some(Instant::now());
            return;
        }
        drop(job);
        tokio::time::sleep(EMIT_INTERVAL).await;
    }
}

impl Manager {
    pub fn new(activity: Activity) -> Self {
        Self { jobs: Vec::new(), next: AtomicU32::new(0), activity }
    }

    async fn start(&mut self, conn: &Connection, kind: &'static str, request: Request) -> zbus::fdo::Result<OwnedObjectPath> {
        let (total_files, total_bytes) = match &request {
            Request::Copy(sources, _) | Request::Move(sources, _) | Request::Trash(sources) | Request::Delete(sources) => {
                ops::measure(sources)
            }
            Request::Undo(steps, _) => (steps.len() as u64, 0),
        };
        let shared = Arc::new(Shared {
            kind,
            total_files,
            total_bytes,
            status: Mutex::new(Status {
                state: State::Running,
                done_files: 0,
                done_bytes: 0,
                current: PathBuf::new(),
                error: String::new(),
                conflict: None,
                undo: None,
            }),
            cancel: AtomicBool::new(false),
            answer: Mutex::new(None),
            changed: Notify::new(),
        });
        let n = self.next.fetch_add(1, Ordering::Relaxed);
        let path = OwnedObjectPath::try_from(format!("{PATH}/Job/{n}")).map_err(|e| zbus::fdo::Error::Failed(e.to_string()))?;
        conn.object_server().at(&path, Job { shared: shared.clone() }).await?;
        lock(&self.activity).0 += 1;
        tokio::spawn(publish(conn.clone(), path.clone(), shared.clone(), self.activity.clone()));
        let worker = shared.clone();
        tokio::task::spawn_blocking(move || run(request, &worker));
        self.jobs.push((path.clone(), shared));
        log::info!("{kind} job {n} started");
        Ok(path)
    }
}

#[interface(name = "org.andromeda.FileOps1")]
impl Manager {
    #[zbus(property)]
    fn jobs(&self) -> Vec<OwnedObjectPath> {
        self.jobs.iter().map(|(path, _)| path.clone()).collect()
    }

    /// Copies into the folder `destination`. Paths are absolute, as bytes.
    async fn copy(
        &mut self,
        sources: Vec<Vec<u8>>,
        destination: Vec<u8>,
        #[zbus(connection)] conn: &Connection,
        #[zbus(signal_emitter)] emitter: zbus::object_server::SignalEmitter<'_>,
    ) -> zbus::fdo::Result<OwnedObjectPath> {
        let dir = paths(vec![destination])?.remove(0);
        let path = self.start(conn, "copy", Request::Copy(paths(sources)?, dir)).await?;
        self.jobs_changed(&emitter).await?;
        Ok(path)
    }

    async fn r#move(
        &mut self,
        sources: Vec<Vec<u8>>,
        destination: Vec<u8>,
        #[zbus(connection)] conn: &Connection,
        #[zbus(signal_emitter)] emitter: zbus::object_server::SignalEmitter<'_>,
    ) -> zbus::fdo::Result<OwnedObjectPath> {
        let dir = paths(vec![destination])?.remove(0);
        let path = self.start(conn, "move", Request::Move(paths(sources)?, dir)).await?;
        self.jobs_changed(&emitter).await?;
        Ok(path)
    }

    async fn trash(
        &mut self,
        sources: Vec<Vec<u8>>,
        #[zbus(connection)] conn: &Connection,
        #[zbus(signal_emitter)] emitter: zbus::object_server::SignalEmitter<'_>,
    ) -> zbus::fdo::Result<OwnedObjectPath> {
        let path = self.start(conn, "trash", Request::Trash(paths(sources)?)).await?;
        self.jobs_changed(&emitter).await?;
        Ok(path)
    }

    /// Deletes for good; the job has nothing to undo.
    async fn delete(
        &mut self,
        sources: Vec<Vec<u8>>,
        #[zbus(connection)] conn: &Connection,
        #[zbus(signal_emitter)] emitter: zbus::object_server::SignalEmitter<'_>,
    ) -> zbus::fdo::Result<OwnedObjectPath> {
        let path = self.start(conn, "delete", Request::Delete(paths(sources)?)).await?;
        self.jobs_changed(&emitter).await?;
        Ok(path)
    }

    /// Puts trashed items back, each given as (trash folder, name in its `files/`); also items another program
    /// trashed. A taken original path gets a "(copy)" name.
    async fn restore(
        &mut self,
        items: Vec<(Vec<u8>, String)>,
        #[zbus(connection)] conn: &Connection,
        #[zbus(signal_emitter)] emitter: zbus::object_server::SignalEmitter<'_>,
    ) -> zbus::fdo::Result<OwnedObjectPath> {
        let steps = items
            .into_iter()
            .map(|(dir, name)| {
                let dir = paths(vec![dir])?.remove(0);
                if name.is_empty() || name == "." || name == ".." || name.contains(['/', '\0']) {
                    return Err(zbus::fdo::Error::InvalidArgs(format!("bad trash name '{name}'")));
                }
                trash::read_info(&dir, &name)
                    .map(UndoStep::Trashed)
                    .map_err(|e| zbus::fdo::Error::Failed(format!("{}: {e}", dir.display())))
            })
            .collect::<zbus::fdo::Result<Vec<_>>>()?;
        let path = self.start(conn, "restore", Request::Undo(steps, None)).await?;
        self.jobs_changed(&emitter).await?;
        Ok(path)
    }

    /// Reverses a finished job (also a failed or cancelled one, as far as it got); the new job can be undone in
    /// turn (redo). A job undoes once.
    async fn undo(
        &mut self,
        job: ObjectPath<'_>,
        #[zbus(connection)] conn: &Connection,
        #[zbus(signal_emitter)] emitter: zbus::object_server::SignalEmitter<'_>,
    ) -> zbus::fdo::Result<OwnedObjectPath> {
        let shared = self
            .jobs
            .iter()
            .find(|(path, _)| path.as_ref() == job)
            .map(|(_, shared)| shared.clone())
            .ok_or_else(|| zbus::fdo::Error::UnknownObject(job.to_string()))?;
        let steps = {
            let mut status = lock(&shared.status);
            if matches!(status.state, State::Running | State::Conflict) {
                return Err(zbus::fdo::Error::Failed("job still runs".into()));
            }
            status.undo.take().filter(|steps| !steps.is_empty())
        }
        .ok_or_else(|| zbus::fdo::Error::Failed("nothing to undo".into()))?;
        if let Ok(iface) = conn.object_server().interface::<_, Job>(&job).await {
            let _ = iface.get().await.undoable_changed(iface.signal_emitter()).await;
        }
        let path = self.start(conn, "undo", Request::Undo(steps, Some(shared))).await?;
        self.jobs_changed(&emitter).await?;
        Ok(path)
    }
}
