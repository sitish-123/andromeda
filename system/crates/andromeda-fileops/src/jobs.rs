//! `org.andromeda.FileOps1`: one shared job list for every app. Each job runs on its own blocking thread and is an
//! object `/org/andromeda/FileOps1/Job/<n>` with live properties, so any window shows and answers it.

use std::ffi::OsString;
use std::os::unix::ffi::{OsStrExt, OsStringExt};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, AtomicU32, AtomicU64, Ordering};
use std::sync::mpsc;
use std::sync::{Arc, Condvar, Mutex, MutexGuard, PoisonError};
use std::time::{Duration, Instant};

use tokio::sync::Notify;
use zbus::object_server::InterfaceRef;
use zbus::{Connection, ObjectPath, OwnedObjectPath, interface};

use crate::archive;
use crate::info;
use crate::ops::{self, Control, OpError, Resolution, UndoStep};
use crate::storage;
use crate::tags;
use crate::trash;

/// Errors of `ListArchive` a client acts on (a password prompt).
#[derive(Debug, zbus::DBusError)]
#[zbus(prefix = "org.andromeda.FileOps1.Error")]
pub enum ArchiveDbusError {
    #[zbus(error)]
    ZBus(zbus::Error),
    PasswordNeeded(String),
    Unsupported(String),
    Unsafe(String),
    Failed(String),
}

impl From<archive::ArchiveError> for ArchiveDbusError {
    fn from(e: archive::ArchiveError) -> Self {
        let text = e.to_string();
        match e {
            archive::ArchiveError::PasswordNeeded | archive::ArchiveError::WrongPassword => Self::PasswordNeeded(text),
            archive::ArchiveError::Unsupported => Self::Unsupported(text),
            archive::ArchiveError::Unsafe(_) => Self::Unsafe(text),
            _ => Self::Failed(text),
        }
    }
}

/// Inner archive paths from the bus: relative, checked like archive entries.
fn checked_inner_paths(raw: Vec<Vec<u8>>) -> zbus::fdo::Result<Vec<PathBuf>> {
    raw.into_iter()
        .map(|mut p| {
            if p.last() == Some(&0) {
                p.pop();
            }
            archive::checked_path(&p).ok().flatten().ok_or_else(|| {
                zbus::fdo::Error::InvalidArgs(format!("bad inner path '{}'", String::from_utf8_lossy(&p)))
            })
        })
        .collect()
}

pub const PATH: &str = "/org/andromeda/FileOps1";
const EMIT_INTERVAL: Duration = Duration::from_millis(100);
const ANSWER_POLL: Duration = Duration::from_millis(250);

/// Set on SIGTERM: every job stops at its next check, also one that waits for a conflict answer.
pub static SHUTDOWN: AtomicBool = AtomicBool::new(false);

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum State {
    Running,
    /// Pause asked: the worker waits at its next pause point (between chunks and items).
    Paused,
    Conflict,
    Done,
    Failed,
    Cancelled,
}

impl State {
    fn name(self) -> &'static str {
        match self {
            Self::Running => "running",
            Self::Paused => "paused",
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
    /// Set with `conflict` when both sides are folders.
    clash: Option<ops::FolderClash>,
    undo: Option<Vec<UndoStep>>,
    /// What a finished job made: the hex digest of a checksum job.
    result: String,
}

type Answer = Option<(Resolution, bool)>;

struct Shared {
    kind: &'static str,
    /// The folder a copy, move, link or extract writes into; empty for other jobs.
    destination: PathBuf,
    /// Set at start; an Extract sets them on its worker thread (reading the archive index blocks).
    total_files: AtomicU64,
    total_bytes: AtomicU64,
    status: Mutex<Status>,
    /// Signalled with `status` held when a pause ends or the job is cancelled.
    resumed: Condvar,
    paused: AtomicBool,
    /// The conflict question being asked (1-based; 0 before the first) and how many the job asks (0 = unknown).
    conflict_index: AtomicU64,
    conflict_total: AtomicU64,
    cancel: AtomicBool,
    answer: Mutex<Option<mpsc::Sender<Answer>>>,
    changed: Notify,
}

fn lock<T>(m: &Mutex<T>) -> MutexGuard<'_, T> {
    m.lock().unwrap_or_else(PoisonError::into_inner)
}

impl Shared {
    fn pause(&self) -> Result<(), String> {
        let mut status = lock(&self.status);
        if status.state != State::Running {
            return Err(format!("a {} job cannot pause", status.state.name()));
        }
        self.paused.store(true, Ordering::Relaxed);
        status.state = State::Paused;
        drop(status);
        self.changed.notify_one();
        Ok(())
    }

    fn resume(&self) -> Result<(), String> {
        let mut status = lock(&self.status);
        if status.state != State::Paused {
            return Err("the job is not paused".into());
        }
        self.paused.store(false, Ordering::Relaxed);
        status.state = State::Running;
        self.resumed.notify_all();
        drop(status);
        self.changed.notify_one();
        Ok(())
    }
}

impl Control for Shared {
    fn cancelled(&self) -> bool {
        self.cancel.load(Ordering::Relaxed) || SHUTDOWN.load(Ordering::Relaxed)
    }

    fn pause_point(&self) {
        let mut status = lock(&self.status);
        // A timeout as well: SIGTERM sets SHUTDOWN without the condvar.
        while self.paused.load(Ordering::Relaxed) && !self.cancelled() {
            status = self.resumed.wait_timeout(status, ANSWER_POLL).unwrap_or_else(PoisonError::into_inner).0;
        }
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
        // Measured before the question shows, so the sheet opens with its counts.
        let clash = ops::folder_clash(source, target);
        let index = self.conflict_index.fetch_add(1, Ordering::Relaxed) + 1;
        // A count made at start can fall short (files appear while the job runs): never show k of fewer than k.
        self.conflict_total
            .fetch_update(Ordering::Relaxed, Ordering::Relaxed, |t| (t != 0 && t < index).then_some(index))
            .ok();
        *lock(&self.answer) = Some(tx);
        {
            let mut status = lock(&self.status);
            status.state = State::Conflict;
            status.conflict = Some((source.to_path_buf(), target.to_path_buf()));
            status.clash = clash;
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
        status.state = if self.paused.load(Ordering::Relaxed) { State::Paused } else { State::Running };
        status.conflict = None;
        status.clash = None;
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
    #[zbus(property)]
    fn total_files(&self) -> u64 {
        self.shared.total_files.load(Ordering::Relaxed)
    }
    #[zbus(property)]
    fn total_bytes(&self) -> u64 {
        self.shared.total_bytes.load(Ordering::Relaxed)
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
    fn destination(&self) -> Vec<u8> {
        bytes(&self.shared.destination)
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
    /// (k, m): the conflict question being asked (1-based, 0 before the first) and how many the job asks when
    /// every folder clash merges (0 = unknown).
    #[zbus(property)]
    fn conflict_progress(&self) -> (u64, u64) {
        (self.shared.conflict_index.load(Ordering::Relaxed), self.shared.conflict_total.load(Ordering::Relaxed))
    }
    /// While a folder clashes with a folder: (new, same, different, up to 20 differing (path inside, hint)), where
    /// same = equal size and modification time and a hint reads like "newer here, larger there". Else all empty.
    #[zbus(property)]
    fn folder_clash(&self) -> (u64, u64, u64, Vec<(Vec<u8>, String)>) {
        lock(&self.shared.status).clash.as_ref().map_or_else(Default::default, |c| {
            (c.new, c.same, c.different, c.names.iter().map(|(p, h)| (bytes(p), h.clone())).collect())
        })
    }
    #[zbus(property)]
    fn result(&self) -> String {
        lock(&self.shared.status).result.clone()
    }
    /// What a finished job moved or renamed, as (from, to): clients that keep paths (starred, pins) follow them.
    #[zbus(property)]
    fn moves(&self) -> Vec<(Vec<u8>, Vec<u8>)> {
        let status = lock(&self.shared.status);
        status
            .undo
            .iter()
            .flatten()
            .filter_map(|step| match step {
                UndoStep::Moved { from, to } => Some((bytes(from), bytes(to))),
                _ => None,
            })
            .collect()
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
        let _status = lock(&self.shared.status);
        self.shared.resumed.notify_all();
    }

    /// Only a running job pauses; it stops at its next chunk or item. State becomes paused at once.
    fn pause(&self) -> zbus::fdo::Result<()> {
        self.shared.pause().map_err(zbus::fdo::Error::Failed)
    }

    fn resume(&self) -> zbus::fdo::Result<()> {
        self.shared.resume().map_err(zbus::fdo::Error::Failed)
    }

    /// `resolution`: replace (folders merge; a replaced file goes to the trash), skip or `keep_both`.
    fn resolve_conflict(&self, resolution: &str, apply_to_all: bool) -> zbus::fdo::Result<()> {
        let resolution = Resolution::parse(resolution)
            .ok_or_else(|| zbus::fdo::Error::InvalidArgs(format!("unknown resolution '{resolution}'")))?;
        let tx =
            lock(&self.shared.answer).take().ok_or_else(|| zbus::fdo::Error::Failed("no conflict waits".into()))?;
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
    Rename(PathBuf, String),
    RenameMany(Vec<(PathBuf, String)>),
    NewFolder(PathBuf, String),
    Link(Vec<PathBuf>, PathBuf),
    /// Steps, and the job they came from: steps an undo did not finish go back to it for a retry.
    Undo(Vec<UndoStep>, Option<Arc<Shared>>),
    /// Archive, inner paths (empty = all), destination folder, password.
    Extract(PathBuf, Vec<PathBuf>, PathBuf, String),
    /// Sources, archive path, format, password.
    Compress(Vec<PathBuf>, PathBuf, archive::OutFormat, String, u64),
    /// Paths, mode, mask, recursive.
    SetPermissions(Vec<PathBuf>, u32, u32, bool),
    /// Path, algorithm.
    Checksum(PathBuf, String),
    /// Root, walk again even when a recent scan exists.
    StorageScan(PathBuf, bool),
    /// Paths, tags, mode.
    SetTags(Vec<PathBuf>, Vec<String>, tags::Mode),
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
            if path.is_absolute() {
                Ok(path)
            } else {
                Err(zbus::fdo::Error::InvalidArgs("paths must be absolute".into()))
            }
        })
        .collect()
}

fn run(request: Request, shared: &Shared) {
    let (undo, result) = match request {
        Request::Copy(sources, dir) => ops::copy(&sources, &dir, shared),
        Request::Move(sources, dir) => ops::move_to(&sources, &dir, shared),
        Request::Trash(sources) => ops::trash_all(&sources, shared),
        Request::Delete(sources) => (Vec::new(), ops::delete(&sources, shared)),
        Request::Rename(path, name) => ops::rename(&path, &name, shared),
        Request::RenameMany(items) => ops::rename_many(&items, shared),
        Request::NewFolder(dir, name) => ops::new_folder(&dir, &name, shared),
        Request::Link(sources, dir) => ops::link(&sources, &dir, shared),
        Request::Extract(path, inner, dir, password) => {
            let (files, bytes) = archive::totals(&path);
            shared.total_files.store(files, Ordering::Relaxed);
            shared.total_bytes.store(bytes, Ordering::Relaxed);
            shared.changed.notify_one();
            archive::extract(&path, &inner, &dir, &password, shared)
        }
        Request::Compress(sources, path, format, password, volume) => {
            archive::compress(&sources, &path, format, &password, volume, shared)
        }
        Request::SetPermissions(paths, mode, mask, recursive) => {
            info::set_permissions(&paths, mode, mask, recursive, shared)
        }
        Request::Checksum(path, algorithm) => match info::checksum(&path, &algorithm, shared) {
            Ok(hex) => {
                lock(&shared.status).result = hex;
                (Vec::new(), Ok(()))
            }
            Err(e) => (Vec::new(), Err(e)),
        },
        Request::SetTags(paths, list, mode) => tags::set_tags(&paths, &list, mode, shared),
        Request::StorageScan(root, fresh) => match storage::scan(&root, fresh, shared) {
            Ok(summary) => {
                lock(&shared.status).result = serde_json::to_string(&summary).unwrap_or_default();
                (Vec::new(), Ok(()))
            }
            Err(e) => (Vec::new(), Err(e)),
        },
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
        // Read before the emits: a job that ends after state_changed read "running" must loop once more, else
        // its final State is never sent.
        let finished = matches!(lock(&shared.status).state, State::Done | State::Failed | State::Cancelled);
        let job = iface.get().await;
        let emitter = iface.signal_emitter();
        let _ = job.state_changed(emitter).await;
        let _ = job.total_files_changed(emitter).await;
        let _ = job.total_bytes_changed(emitter).await;
        let _ = job.done_files_changed(emitter).await;
        let _ = job.done_bytes_changed(emitter).await;
        let _ = job.current_changed(emitter).await;
        let _ = job.conflict_changed(emitter).await;
        let _ = job.conflict_progress_changed(emitter).await;
        let _ = job.folder_clash_changed(emitter).await;
        if finished {
            let _ = job.error_changed(emitter).await;
            let _ = job.undoable_changed(emitter).await;
            let _ = job.moves_changed(emitter).await;
            let _ = job.result_changed(emitter).await;
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

    async fn start(
        &mut self,
        conn: &Connection,
        kind: &'static str,
        request: Request,
    ) -> zbus::fdo::Result<OwnedObjectPath> {
        let (total_files, total_bytes) = match &request {
            Request::Copy(sources, _)
            | Request::Move(sources, _)
            | Request::Trash(sources)
            | Request::Delete(sources) => ops::measure(sources),
            Request::Undo(steps, _) => (steps.len() as u64, 0),
            Request::Link(sources, _) => (sources.len() as u64, 0),
            Request::Rename(..) | Request::NewFolder(..) => (1, 0),
            Request::RenameMany(items) => (items.len() as u64, 0),
            Request::Extract(..) => (0, 0),
            Request::Compress(sources, ..) => ops::measure(sources),
            Request::SetPermissions(paths, ..) => (paths.len() as u64, 0),
            Request::Checksum(path, _) => (1, std::fs::metadata(path).map_or(0, |m| m.len())),
            Request::StorageScan(..) => (0, 0),
            Request::SetTags(paths, ..) => (paths.len() as u64, 0),
        };
        let conflict_total = match &request {
            Request::Copy(sources, dir) | Request::Move(sources, dir) => {
                ops::count_conflicts(sources, dir).unwrap_or(0)
            }
            _ => 0,
        };
        let destination = match &request {
            Request::Copy(_, dir) | Request::Move(_, dir) | Request::Link(_, dir) | Request::Extract(_, _, dir, _) => {
                dir.clone()
            }
            _ => PathBuf::new(),
        };
        let shared = Arc::new(Shared {
            kind,
            destination,
            total_files: AtomicU64::new(total_files),
            total_bytes: AtomicU64::new(total_bytes),
            status: Mutex::new(Status {
                state: State::Running,
                done_files: 0,
                done_bytes: 0,
                current: PathBuf::new(),
                error: String::new(),
                conflict: None,
                clash: None,
                undo: None,
                result: String::new(),
            }),
            resumed: Condvar::new(),
            paused: AtomicBool::new(false),
            conflict_index: AtomicU64::new(0),
            conflict_total: AtomicU64::new(conflict_total),
            cancel: AtomicBool::new(false),
            answer: Mutex::new(None),
            changed: Notify::new(),
        });
        let n = self.next.fetch_add(1, Ordering::Relaxed);
        let path = OwnedObjectPath::try_from(format!("{PATH}/Job/{n}"))
            .map_err(|e| zbus::fdo::Error::Failed(e.to_string()))?;
        conn.object_server().at(&path, Job { shared: shared.clone() }).await?;
        lock(&self.activity).0 += 1;
        tokio::spawn(publish(conn.clone(), path.clone(), shared.clone(), self.activity.clone()));
        let worker = shared.clone();
        tokio::task::spawn_blocking(move || {
            // A panic must end the job as failed: else it stays "running" and the app waits for good.
            if let Err(panic) = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| run(request, &worker))) {
                let message = panic
                    .downcast_ref::<String>()
                    .cloned()
                    .or_else(|| panic.downcast_ref::<&str>().map(|s| (*s).to_owned()))
                    .unwrap_or_default();
                log::error!("{kind} job {n} panicked: {message}");
                let mut status = lock(&worker.status);
                status.state = State::Failed;
                status.error = format!("internal error: {message}");
                drop(status);
                worker.changed.notify_one();
            } else {
                log::info!("{kind} job {n} finished: {:?}", lock(&worker.status).state);
            }
        });
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

    /// Renames `path` in its folder to `name` (one file name, no "/"); a taken name fails the job.
    async fn rename(
        &mut self,
        path: Vec<u8>,
        name: String,
        #[zbus(connection)] conn: &Connection,
        #[zbus(signal_emitter)] emitter: zbus::object_server::SignalEmitter<'_>,
    ) -> zbus::fdo::Result<OwnedObjectPath> {
        if !ops::valid_name(&name) {
            return Err(zbus::fdo::Error::InvalidArgs(format!("bad name '{name}'")));
        }
        let path = paths(vec![path])?.remove(0);
        let job = self.start(conn, "rename", Request::Rename(path, name)).await?;
        self.jobs_changed(&emitter).await?;
        Ok(job)
    }

    /// Renames many items in one job (batch rename): all or none, one undo step.
    async fn rename_many(
        &mut self,
        items: Vec<(Vec<u8>, String)>,
        #[zbus(connection)] conn: &Connection,
        #[zbus(signal_emitter)] emitter: zbus::object_server::SignalEmitter<'_>,
    ) -> zbus::fdo::Result<OwnedObjectPath> {
        let mut out = Vec::with_capacity(items.len());
        for (path, name) in items {
            if !ops::valid_name(&name) {
                return Err(zbus::fdo::Error::InvalidArgs(format!("bad name '{name}'")));
            }
            out.push((paths(vec![path])?.remove(0), name));
        }
        if out.is_empty() {
            return Err(zbus::fdo::Error::InvalidArgs("nothing to rename".into()));
        }
        let job = self.start(conn, "rename", Request::RenameMany(out)).await?;
        self.jobs_changed(&emitter).await?;
        Ok(job)
    }

    /// Makes a folder `name` in `destination`; a taken name gets a "(copy)" name.
    async fn new_folder(
        &mut self,
        destination: Vec<u8>,
        name: String,
        #[zbus(connection)] conn: &Connection,
        #[zbus(signal_emitter)] emitter: zbus::object_server::SignalEmitter<'_>,
    ) -> zbus::fdo::Result<OwnedObjectPath> {
        if !ops::valid_name(&name) {
            return Err(zbus::fdo::Error::InvalidArgs(format!("bad name '{name}'")));
        }
        let dir = paths(vec![destination])?.remove(0);
        let job = self.start(conn, "new_folder", Request::NewFolder(dir, name)).await?;
        self.jobs_changed(&emitter).await?;
        Ok(job)
    }

    /// Puts a symbolic link to each source into the folder `destination`.
    async fn link(
        &mut self,
        sources: Vec<Vec<u8>>,
        destination: Vec<u8>,
        #[zbus(connection)] conn: &Connection,
        #[zbus(signal_emitter)] emitter: zbus::object_server::SignalEmitter<'_>,
    ) -> zbus::fdo::Result<OwnedObjectPath> {
        let dir = paths(vec![destination])?.remove(0);
        let job = self.start(conn, "link", Request::Link(paths(sources)?, dir)).await?;
        self.jobs_changed(&emitter).await?;
        Ok(job)
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

    /// Sets permission bits: `mode` where `mask` is set; on folders, open follows read. `recursive` also changes
    /// everything inside folders. Links are left alone. Undo puts the old bits back.
    async fn set_permissions(
        &mut self,
        targets: Vec<Vec<u8>>,
        mode: u32,
        mask: u32,
        recursive: bool,
        #[zbus(connection)] conn: &Connection,
        #[zbus(signal_emitter)] emitter: zbus::object_server::SignalEmitter<'_>,
    ) -> zbus::fdo::Result<OwnedObjectPath> {
        let paths = paths(targets)?;
        if paths.is_empty() || mask & !0o7777 != 0 {
            return Err(zbus::fdo::Error::InvalidArgs("no paths, or a mask beyond 0o7777".into()));
        }
        let job =
            self.start(conn, "permissions", Request::SetPermissions(paths, mode & 0o7777, mask, recursive)).await?;
        self.jobs_changed(&emitter).await?;
        Ok(job)
    }

    /// Digest of one file as a job (progress, Cancel); algorithm sha256 or md5. The hex lands in the job's Result.
    async fn checksum(
        &mut self,
        path: Vec<u8>,
        algorithm: String,
        #[zbus(connection)] conn: &Connection,
        #[zbus(signal_emitter)] emitter: zbus::object_server::SignalEmitter<'_>,
    ) -> zbus::fdo::Result<OwnedObjectPath> {
        if algorithm != "sha256" && algorithm != "md5" {
            return Err(zbus::fdo::Error::InvalidArgs(format!("unknown algorithm '{algorithm}'")));
        }
        let path = paths(vec![path])?.remove(0);
        let job = self.start(conn, "checksum", Request::Checksum(path, algorithm)).await?;
        self.jobs_changed(&emitter).await?;
        Ok(job)
    }

    /// Changes tags in `user.xdg.tags`: mode set (exactly these), add, remove or rename ([old, new]). Undo puts back each old value.
    async fn set_tags(
        &mut self,
        targets: Vec<Vec<u8>>,
        tags: Vec<String>,
        mode: String,
        #[zbus(connection)] conn: &Connection,
        #[zbus(signal_emitter)] emitter: zbus::object_server::SignalEmitter<'_>,
    ) -> zbus::fdo::Result<OwnedObjectPath> {
        let mode =
            tags::Mode::parse(&mode).ok_or_else(|| zbus::fdo::Error::InvalidArgs(format!("unknown mode '{mode}'")))?;
        if let Some(bad) = tags.iter().find(|t| !tags::valid_tag(t)) {
            return Err(zbus::fdo::Error::InvalidArgs(format!("bad tag '{bad}'")));
        }
        if mode == tags::Mode::Rename && tags.len() != 2 {
            return Err(zbus::fdo::Error::InvalidArgs("rename takes [old, new]".into()));
        }
        let paths = paths(targets)?;
        if paths.is_empty() {
            return Err(zbus::fdo::Error::InvalidArgs("nothing to tag".into()));
        }
        let list = tags.into_iter().map(|t| t.trim().to_owned()).collect();
        let job = self.start(conn, "tags", Request::SetTags(paths, list, mode)).await?;
        self.jobs_changed(&emitter).await?;
        Ok(job)
    }

    /// Scans `root` (one file system) as a job: its Result is JSON {root, total, categories [[name, bytes]],
    /// largest [[path, bytes]], trash, caches, old_downloads, old_download_items [[path, bytes]], scanned_at,
    /// cached (read from disk after a daemon restart)}. A scan
    /// younger than 10 min is reused unless `fresh`.
    async fn storage_scan(
        &mut self,
        root: Vec<u8>,
        fresh: bool,
        #[zbus(connection)] conn: &Connection,
        #[zbus(signal_emitter)] emitter: zbus::object_server::SignalEmitter<'_>,
    ) -> zbus::fdo::Result<OwnedObjectPath> {
        let root = paths(vec![root])?.remove(0);
        let job = self.start(conn, "storage_scan", Request::StorageScan(root, fresh)).await?;
        self.jobs_changed(&emitter).await?;
        Ok(job)
    }

    /// Folders and files in `path` with their bytes on disk, largest first: (path, bytes, `is_dir`). Folder sizes
    /// come from the last `StorageScan` that covers `path`. Runs off the bus thread.
    async fn storage_folder(&self, path: Vec<u8>) -> zbus::fdo::Result<Vec<(Vec<u8>, u64, bool)>> {
        let path = paths(vec![path])?.remove(0);
        let items = tokio::task::spawn_blocking(move || storage::folder(&path))
            .await
            .map_err(|e| zbus::fdo::Error::Failed(e.to_string()))?
            .map_err(|e| zbus::fdo::Error::Failed(e.to_string()))?;
        Ok(items.into_iter().map(|(p, size, dir)| (bytes(&p), size, dir)).collect())
    }

    /// Entries of an archive: (inner path, kind `file`, `dir` or `link`, size, mtime in s, `is_dir`). Runs off the bus thread.
    async fn list_archive(&self, path: Vec<u8>) -> Result<Vec<(Vec<u8>, String, i64, u64, bool)>, ArchiveDbusError> {
        let path = paths(vec![path]).map_err(|e| ArchiveDbusError::Failed(e.to_string()))?.remove(0);
        let entries = tokio::task::spawn_blocking(move || archive::list(&path))
            .await
            .map_err(|e| ArchiveDbusError::Failed(e.to_string()))??;
        Ok(entries
            .into_iter()
            .map(|e| {
                let size = i64::try_from(e.size).unwrap_or(i64::MAX);
                (bytes(&e.path), e.kind.name().to_owned(), size, e.mtime, e.kind == archive::Kind::Dir)
            })
            .collect())
    }

    /// Extracts `inner_paths` (relative; empty = all) into the folder `destination`.
    async fn extract(
        &mut self,
        archive: Vec<u8>,
        inner_paths: Vec<Vec<u8>>,
        destination: Vec<u8>,
        password: String,
        #[zbus(connection)] conn: &Connection,
        #[zbus(signal_emitter)] emitter: zbus::object_server::SignalEmitter<'_>,
    ) -> zbus::fdo::Result<OwnedObjectPath> {
        let mut both = paths(vec![archive, destination])?;
        let dir = both.remove(1);
        let request = Request::Extract(both.remove(0), checked_inner_paths(inner_paths)?, dir, password);
        let job = self.start(conn, "extract", request).await?;
        self.jobs_changed(&emitter).await?;
        Ok(job)
    }

    /// Writes `sources` into the new archive `destination`; format zip, 7z, tar.gz, tar.xz or tar.zst. Only zip and
    /// 7z take a password. A `volume_size` above 0 splits a larger archive into destination.001, .002, …
    async fn compress(
        &mut self,
        sources: Vec<Vec<u8>>,
        destination: Vec<u8>,
        format: String,
        password: String,
        volume_size: u64,
        #[zbus(connection)] conn: &Connection,
        #[zbus(signal_emitter)] emitter: zbus::object_server::SignalEmitter<'_>,
    ) -> zbus::fdo::Result<OwnedObjectPath> {
        let format = archive::OutFormat::parse(&format)
            .ok_or_else(|| zbus::fdo::Error::InvalidArgs(format!("unknown format '{format}'")))?;
        if !password.is_empty() && !format.takes_password() {
            return Err(zbus::fdo::Error::InvalidArgs("only zip and 7z take a password".into()));
        }
        let sources = paths(sources)?;
        if sources.is_empty() {
            return Err(zbus::fdo::Error::InvalidArgs("nothing to compress".into()));
        }
        let target = paths(vec![destination])?.remove(0);
        let job =
            self.start(conn, "compress", Request::Compress(sources, target, format, password, volume_size)).await?;
        self.jobs_changed(&emitter).await?;
        Ok(job)
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

#[cfg(test)]
#[allow(clippy::expect_used)]
mod tests {
    use super::*;

    fn shared(total: u64) -> Arc<Shared> {
        Arc::new(Shared {
            kind: "copy",
            destination: PathBuf::new(),
            total_files: AtomicU64::new(0),
            total_bytes: AtomicU64::new(0),
            status: Mutex::new(Status {
                state: State::Running,
                done_files: 0,
                done_bytes: 0,
                current: PathBuf::new(),
                error: String::new(),
                conflict: None,
                clash: None,
                undo: None,
                result: String::new(),
            }),
            resumed: Condvar::new(),
            paused: AtomicBool::new(false),
            conflict_index: AtomicU64::new(0),
            conflict_total: AtomicU64::new(total),
            cancel: AtomicBool::new(false),
            answer: Mutex::new(None),
            changed: Notify::new(),
        })
    }

    #[test]
    fn a_paused_job_waits_at_its_pause_point_until_resume() {
        let job = shared(0);
        job.pause().expect("pause");
        assert!(lock(&job.status).state == State::Paused);
        let (tx, rx) = mpsc::channel();
        let worker = job.clone();
        let thread = std::thread::spawn(move || {
            worker.pause_point();
            tx.send(()).expect("send");
        });
        assert!(rx.recv_timeout(Duration::from_millis(300)).is_err(), "the worker ran on while paused");
        job.resume().expect("resume");
        rx.recv_timeout(Duration::from_secs(2)).expect("the worker went on after resume");
        thread.join().expect("join");
        assert!(lock(&job.status).state == State::Running);
        assert!(job.resume().is_err(), "a running job cannot resume");
    }

    #[test]
    fn cancel_ends_a_pause() {
        let job = shared(0);
        job.pause().expect("pause");
        let worker = job.clone();
        let thread = std::thread::spawn(move || worker.pause_point());
        Job { shared: job.clone() }.cancel();
        thread.join().expect("the pause ended on cancel");
        assert!(job.cancelled());
    }

    #[test]
    fn conflicts_count_k_of_m_and_carry_the_folder_clash() {
        let root = tempfile::tempdir().expect("tmp");
        let (src, dst) = (root.path().join("src/d"), root.path().join("dst/d"));
        std::fs::create_dir_all(&src).expect("mkdir");
        std::fs::create_dir_all(&dst).expect("mkdir");
        std::fs::write(src.join("n"), "new").expect("write");
        let job = shared(1);
        let asker = job.clone();
        let (s, d) = (src.clone(), dst.clone());
        let thread = std::thread::spawn(move || asker.conflict(&s, &d));
        // Wait for the question to show.
        let deadline = Instant::now() + Duration::from_secs(2);
        while lock(&job.answer).is_none() {
            assert!(Instant::now() < deadline, "no question");
            std::thread::sleep(Duration::from_millis(10));
        }
        let iface = Job { shared: job.clone() };
        assert_eq!(iface.conflict_progress(), (1, 1));
        let (new, same, different, names) = iface.folder_clash();
        assert_eq!((new, same, different, names.len()), (1, 0, 0, 0));
        iface.resolve_conflict("replace", false).expect("answer");
        assert_eq!(thread.join().expect("join"), Some((Resolution::Replace, false)));
        assert_eq!(iface.folder_clash().0, 0, "the clash clears with the question");
    }
}
