//! File operations without D-Bus: copy, move, trash, restore and delete, with conflict questions, progress,
//! cancellation and the steps that undo them. Every function blocks; the bus layer runs each job on its own thread.

use std::fs::{self, File, OpenOptions};
use std::io;
use std::os::unix::ffi::OsStrExt;
use std::os::unix::fs::{DirBuilderExt, FileExt, MetadataExt, OpenOptionsExt, PermissionsExt, symlink};
use std::path::{Path, PathBuf};

use rustix::fs::{CWD, RenameFlags, Timespec, Timestamps, copy_file_range, futimens, ioctl_ficlone, renameat_with};
use rustix::io::Errno;

use crate::trash::{self, Trashed};

const CHUNK: usize = 8 << 20;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Resolution {
    /// The old target goes to the trash (undo brings it back); a folder is replaced as a whole.
    Replace,
    /// A folder onto a folder: copy into it, asking about each clash inside. On a file it is Replace.
    Merge,
    Skip,
    KeepBoth,
}

impl Resolution {
    pub fn parse(s: &str) -> Option<Self> {
        match s {
            "replace" => Some(Self::Replace),
            "merge" => Some(Self::Merge),
            "skip" => Some(Self::Skip),
            "keep_both" => Some(Self::KeepBoth),
            _ => None,
        }
    }
}

/// What a running operation needs from its job: progress, cancellation and conflict answers.
pub trait Control {
    fn cancelled(&self) -> bool;
    /// Blocks while the job is paused; returns at once when it is not (or when it is cancelled). Operations call
    /// it between chunks and items.
    fn pause_point(&self) {}
    fn progress(&self, files: u64, bytes: u64, current: &Path);
    /// `None` cancels the job. The bool is "apply to all".
    fn conflict(&self, source: &Path, target: &Path) -> Option<(Resolution, bool)>;
}

/// One step that reverses part of a finished operation, run in order.
#[derive(Clone, Debug)]
pub enum UndoStep {
    /// A path the operation created.
    Created(PathBuf),
    /// Moved from `from` to `to`.
    Moved { from: PathBuf, to: PathBuf },
    /// Sent to the trash (by the operation, or replaced by it).
    Trashed(Trashed),
    /// Permission bits before a change (`SetPermissions`).
    Mode { path: PathBuf, mode: u32 },
    /// The `user.xdg.tags` value before a change (`None`: there was none).
    Tags { path: PathBuf, raw: Option<Vec<u8>> },
}

#[derive(Debug)]
pub enum OpError {
    Cancelled,
    Io(PathBuf, io::Error),
}

impl std::fmt::Display for OpError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::Cancelled => write!(f, "cancelled"),
            Self::Io(path, e) => write!(f, "{}: {e}", path.display()),
        }
    }
}

pub type OpResult<T> = Result<T, OpError>;

fn io_at(path: &Path) -> impl FnOnce(io::Error) -> OpError + '_ {
    move |e| OpError::Io(path.to_path_buf(), e)
}

fn errno_at(path: &Path) -> impl FnOnce(Errno) -> OpError + '_ {
    move |e| OpError::Io(path.to_path_buf(), e.into())
}

/// Files and bytes under `paths` (symlinks not followed), for the progress total.
pub fn measure(paths: &[PathBuf]) -> (u64, u64) {
    fn walk(path: &Path, totals: &mut (u64, u64)) {
        let Ok(meta) = fs::symlink_metadata(path) else { return };
        totals.0 += 1;
        if meta.is_dir() {
            if let Ok(entries) = fs::read_dir(path) {
                for entry in entries.flatten() {
                    walk(&entry.path(), totals);
                }
            }
        } else if meta.is_file() {
            totals.1 += meta.len();
        }
    }
    let mut totals = (0, 0);
    for path in paths {
        walk(path, &mut totals);
    }
    totals
}

/// Entries a count or comparison walks before it gives up (the answer is then unknown, or a lower bound).
const WALK_CAP: usize = 100_000;

/// How many conflict questions a copy or move of `sources` into `dir` asks when every folder clash merges:
/// one per clashing name, plus the clashes inside merged folders. `None` past `WALK_CAP` entries.
#[must_use]
pub fn count_conflicts(sources: &[PathBuf], dir: &Path) -> Option<u64> {
    fn walk(source: &Path, dir: &Path, count: &mut u64, seen: &mut usize) -> bool {
        *seen += 1;
        if *seen > WALK_CAP {
            return false;
        }
        let Some(name) = source.file_name() else { return true };
        let target = dir.join(name);
        let Ok(existing) = fs::symlink_metadata(&target) else { return true };
        if same_file(source, &target) {
            return true;
        }
        *count += 1;
        if existing.is_dir() && fs::symlink_metadata(source).is_ok_and(|m| m.is_dir()) {
            let Ok(entries) = fs::read_dir(source) else { return true };
            for entry in entries.flatten() {
                if !walk(&entry.path(), &target, count, seen) {
                    return false;
                }
            }
        }
        true
    }
    let (mut count, mut seen) = (0, 0);
    for source in sources {
        if !walk(source, dir, &mut count, &mut seen) {
            return None;
        }
    }
    Some(count)
}

/// A folder dropped onto a folder of the same name (FILES-UI-SPEC §14): what a merge would do with its files.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct FolderClash {
    /// Files only in the dropped folder.
    pub new: u64,
    /// Files in both with the same size and modification time.
    pub same: u64,
    /// Files in both that differ.
    pub different: u64,
    /// Up to `MAX_CLASH_NAMES` differing files: the path inside the folder and a hint ("newer here, larger there").
    pub names: Vec<(PathBuf, String)>,
}

pub const MAX_CLASH_NAMES: usize = 20;

/// Compares the files under `source` with those under `target` (both folders); `None` when either is not a folder.
/// Past `WALK_CAP` entries the counts cover what was walked.
#[must_use]
pub fn folder_clash(source: &Path, target: &Path) -> Option<FolderClash> {
    fn hint(here: &fs::Metadata, there: &fs::Metadata) -> String {
        let mut parts = Vec::new();
        let (a, b) = ((here.mtime(), here.mtime_nsec()), (there.mtime(), there.mtime_nsec()));
        if a != b {
            parts.push(if a > b { "newer here" } else { "newer there" });
        }
        if here.len() != there.len() {
            parts.push(if here.len() > there.len() { "larger here" } else { "larger there" });
        }
        if parts.is_empty() {
            parts.push("different kind");
        }
        parts.join(", ")
    }
    fn walk(source: &Path, target: &Path, inner: &Path, clash: &mut FolderClash, seen: &mut usize) {
        let Ok(entries) = fs::read_dir(source) else { return };
        for entry in entries.flatten() {
            *seen += 1;
            if *seen > WALK_CAP {
                return;
            }
            let name = entry.file_name();
            let Ok(here) = fs::symlink_metadata(entry.path()) else { continue };
            let there = fs::symlink_metadata(target.join(&name));
            if here.is_dir() {
                match there {
                    Ok(there) if there.is_dir() => walk(&entry.path(), &target.join(&name), &inner.join(&name), clash, seen),
                    // A folder that is new here brings all its files as new.
                    Err(_) => clash.new += measure(&[entry.path()]).0.saturating_sub(1),
                    Ok(there) => {
                        clash.different += 1;
                        if clash.names.len() < MAX_CLASH_NAMES {
                            clash.names.push((inner.join(&name), hint(&here, &there)));
                        }
                    }
                }
                continue;
            }
            match there {
                Err(_) => clash.new += 1,
                Ok(there)
                    if there.file_type() == here.file_type()
                        && there.len() == here.len()
                        && (there.mtime(), there.mtime_nsec()) == (here.mtime(), here.mtime_nsec()) =>
                {
                    clash.same += 1;
                }
                Ok(there) => {
                    clash.different += 1;
                    if clash.names.len() < MAX_CLASH_NAMES {
                        clash.names.push((inner.join(&name), hint(&here, &there)));
                    }
                }
            }
        }
    }
    let both = fs::symlink_metadata(source).is_ok_and(|m| m.is_dir()) && fs::symlink_metadata(target).is_ok_and(|m| m.is_dir());
    if !both {
        return None;
    }
    let mut clash = FolderClash::default();
    let mut seen = 0;
    walk(source, target, Path::new(""), &mut clash, &mut seen);
    Some(clash)
}

/// `name (copy).ext`, `name (copy 2).ext`, ...: the first free name next to `target`.
pub fn free_name(target: &Path) -> PathBuf {
    let parent = target.parent().unwrap_or(Path::new("/"));
    let name = target.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default();
    let is_dir = fs::symlink_metadata(target).is_ok_and(|m| m.is_dir());
    let (stem, ext) = match name.rfind('.') {
        Some(dot) if dot > 0 && !is_dir => (&name[..dot], &name[dot..]),
        _ => (name.as_str(), ""),
    };
    (1..u32::MAX)
        .map(|n| {
            let suffix = if n == 1 { " (copy)".to_owned() } else { format!(" (copy {n})") };
            parent.join(format!("{stem}{suffix}{ext}"))
        })
        .find(|candidate| fs::symlink_metadata(candidate).is_err())
        .unwrap_or_else(|| target.to_path_buf())
}

struct Run<'a> {
    ctl: &'a dyn Control,
    files: u64,
    bytes: u64,
    apply_all: Option<Resolution>,
    undo: Vec<UndoStep>,
}

impl<'a> Run<'a> {
    fn new(ctl: &'a dyn Control) -> Self {
        Self { ctl, files: 0, bytes: 0, apply_all: None, undo: Vec::new() }
    }

    fn check(&self) -> OpResult<()> {
        self.ctl.pause_point();
        if self.ctl.cancelled() { Err(OpError::Cancelled) } else { Ok(()) }
    }

    fn tick(&mut self, files: u64, bytes: u64, current: &Path) {
        self.files += files;
        self.bytes += bytes;
        self.ctl.progress(self.files, self.bytes, current);
    }

    fn resolve(&mut self, source: &Path, target: &Path) -> OpResult<Resolution> {
        if let Some(resolution) = self.apply_all {
            return Ok(resolution);
        }
        let (resolution, all) = self.ctl.conflict(source, target).ok_or(OpError::Cancelled)?;
        if all {
            self.apply_all = Some(resolution);
        }
        Ok(resolution)
    }

    /// Where `source` lands in `dir`, after asking about a conflict; `None` = skip. A folder onto a folder with
    /// Merge goes into it.
    /// `duplicate`: a copy onto itself gets a new name; a move onto itself is skipped.
    fn target_for(&mut self, source: &Path, dir: &Path, duplicate: bool) -> OpResult<Option<(PathBuf, bool)>> {
        let name = source.file_name().ok_or_else(|| OpError::Io(source.into(), io::ErrorKind::InvalidInput.into()))?;
        if fs::canonicalize(dir).is_ok_and(|dir| fs::canonicalize(source).is_ok_and(|src| dir.starts_with(src))) {
            return Err(OpError::Io(source.into(), io::Error::other("cannot copy or move a folder into itself")));
        }
        let target = dir.join(name);
        let Ok(existing) = fs::symlink_metadata(&target) else { return Ok(Some((target, false))) };
        if same_file(source, &target) {
            return Ok(duplicate.then(|| (free_name(&target), false)));
        }
        match self.resolve(source, &target)? {
            Resolution::Skip => Ok(None),
            Resolution::KeepBoth => Ok(Some((free_name(&target), false))),
            resolution @ (Resolution::Replace | Resolution::Merge) => {
                let source_is_dir = fs::symlink_metadata(source).is_ok_and(|m| m.is_dir());
                if resolution == Resolution::Merge && source_is_dir && existing.is_dir() {
                    return Ok(Some((target, true)));
                }
                self.set_aside(source, &target)?;
                Ok(Some((target, false)))
            }
        }
    }

    /// Moves a replaced target to the trash so undo can bring it back. Without a usable trash the job fails:
    /// a replace never deletes for good.
    fn set_aside(&mut self, source: &Path, target: &Path) -> OpResult<()> {
        if source.starts_with(target) {
            return Err(OpError::Io(source.into(), io::Error::other("cannot replace a folder with an item inside it")));
        }
        let trashed = trash::trash(target).map_err(io_at(target))?;
        self.undo.push(UndoStep::Trashed(trashed));
        Ok(())
    }

    fn copy_into(&mut self, source: &Path, dir: &Path) -> OpResult<()> {
        self.check()?;
        let Some((target, merge)) = self.target_for(source, dir, true)? else {
            let (files, bytes) = measure(&[source.to_path_buf()]);
            self.tick(files, bytes, source);
            return Ok(());
        };
        self.copy_to(source, &target, merge, true)
    }

    /// `record`: push an undo step for `target`; false inside a folder this run created (undo takes the folder).
    fn copy_to(&mut self, source: &Path, target: &Path, merge: bool, record: bool) -> OpResult<()> {
        let meta = fs::symlink_metadata(source).map_err(io_at(source))?;
        if meta.is_dir() {
            if !merge {
                // Private until the end: the source's mode applies once the contents are in, and an error or a
                // cancel leaves a folder no one else can read rather than one open by umask.
                fs::DirBuilder::new().mode(0o700).create(target).map_err(io_at(target))?;
                if record {
                    self.undo.push(UndoStep::Created(target.to_path_buf()));
                }
            }
            self.tick(1, 0, source);
            for entry in fs::read_dir(source).map_err(io_at(source))? {
                let entry = entry.map_err(io_at(source))?;
                if merge {
                    self.copy_into(&entry.path(), target)?;
                } else {
                    self.check()?;
                    self.copy_to(&entry.path(), &target.join(entry.file_name()), false, false)?;
                }
            }
            copy_xattrs(source, target);
            fs::set_permissions(target, meta.permissions()).map_err(io_at(target))?;
            if !merge {
                set_times(target, &meta);
            }
        } else if meta.is_symlink() {
            symlink(fs::read_link(source).map_err(io_at(source))?, target).map_err(io_at(target))?;
            if record {
                self.undo.push(UndoStep::Created(target.to_path_buf()));
            }
            self.tick(1, 0, source);
        } else if meta.is_file() {
            self.copy_file(source, target, &meta)?;
            if record {
                self.undo.push(UndoStep::Created(target.to_path_buf()));
            }
            self.tick(1, 0, source);
        } else {
            return Err(OpError::Io(source.into(), io::Error::other("not a regular file, folder or link")));
        }
        Ok(())
    }

    fn copy_file(&mut self, source: &Path, target: &Path, meta: &fs::Metadata) -> OpResult<()> {
        let input = File::open(source).map_err(io_at(source))?;
        let output = OpenOptions::new()
            .write(true)
            .create_new(true)
            .mode(meta.permissions().mode() & 0o7777)
            .open(target)
            .map_err(io_at(target))?;
        // Fewer blocks than bytes: holes, which a plain copy would fill (VM images, databases).
        let sparse = meta.blocks().saturating_mul(512) < meta.len();
        let result = self.copy_data(&input, &output, meta.len(), source, sparse);
        if result.is_err() {
            let _ = fs::remove_file(target);
            return result;
        }
        let times = Timestamps {
            last_access: Timespec { tv_sec: meta.atime(), tv_nsec: meta.atime_nsec() },
            last_modification: Timespec { tv_sec: meta.mtime(), tv_nsec: meta.mtime_nsec() },
        };
        copy_xattrs(source, target);
        futimens(&output, &times).map_err(errno_at(target))
    }

    fn copy_data(&mut self, input: &File, output: &File, len: u64, source: &Path, sparse: bool) -> OpResult<()> {
        if len > 0 && ioctl_ficlone(output, input).is_ok() {
            self.tick(0, len, source);
            return Ok(());
        }
        if sparse {
            return self.copy_sparse(input, output, len, source);
        }
        loop {
            self.check()?;
            match copy_file_range(input, None, output, None, CHUNK) {
                Ok(0) => return Ok(()),
                Ok(n) => self.tick(0, n as u64, source),
                Err(Errno::XDEV | Errno::NOSYS | Errno::INVAL | Errno::OPNOTSUPP) => {
                    return self.copy_plain(input, output, source);
                }
                Err(e) => return Err(OpError::Io(source.into(), e.into())),
            }
        }
    }

    /// Copies only the data runs (SEEK_DATA / SEEK_HOLE) and sets the length, so holes stay holes.
    fn copy_sparse(&mut self, input: &File, output: &File, len: u64, source: &Path) -> OpResult<()> {
        let mut buffer = vec![0u8; 1 << 20];
        let mut pos = 0u64;
        while pos < len {
            let data = match rustix::fs::seek(input, rustix::fs::SeekFrom::Data(pos)) {
                Ok(data) => data,
                Err(Errno::NXIO) => break,
                Err(e) => return Err(OpError::Io(source.into(), e.into())),
            };
            let hole = rustix::fs::seek(input, rustix::fs::SeekFrom::Hole(data)).map_err(errno_at(source))?.min(len);
            let mut at = data;
            while at < hole {
                self.check()?;
                let want = usize::try_from(hole - at).unwrap_or(usize::MAX).min(buffer.len());
                let n = input.read_at(&mut buffer[..want], at).map_err(io_at(source))?;
                if n == 0 {
                    break;
                }
                output.write_all_at(&buffer[..n], at).map_err(io_at(source))?;
                at += n as u64;
                self.tick(0, n as u64, source);
            }
            pos = hole.max(at);
        }
        output.set_len(len).map_err(io_at(source))
    }

    fn copy_plain(&mut self, mut input: &File, mut output: &File, source: &Path) -> OpResult<()> {
        let mut buffer = vec![0u8; 1 << 20];
        loop {
            self.check()?;
            let n = io::Read::read(&mut input, &mut buffer).map_err(io_at(source))?;
            if n == 0 {
                return Ok(());
            }
            io::Write::write_all(&mut output, &buffer[..n]).map_err(io_at(source))?;
            self.tick(0, n as u64, source);
        }
    }

    fn move_into(&mut self, source: &Path, dir: &Path) -> OpResult<()> {
        self.check()?;
        let Some((target, merge)) = self.target_for(source, dir, false)? else {
            let (files, bytes) = measure(&[source.to_path_buf()]);
            self.tick(files, bytes, source);
            return Ok(());
        };
        if merge {
            for entry in fs::read_dir(source).map_err(io_at(source))? {
                self.move_into(&entry.map_err(io_at(source))?.path(), &target)?;
            }
            let _ = fs::remove_dir(source);
            return Ok(());
        }
        let (files, bytes) = measure(&[source.to_path_buf()]);
        match renameat_with(CWD, source, CWD, &target, RenameFlags::NOREPLACE) {
            Ok(()) => {
                self.undo.push(UndoStep::Moved { from: source.to_path_buf(), to: target });
                self.tick(files, bytes, source);
                Ok(())
            }
            Err(Errno::XDEV) => {
                let mark = self.undo.len();
                self.copy_to(source, &target, false, true)?;
                remove_all(source).map_err(io_at(source))?;
                // Undo of a cross-device move is a move back, not removal of what the copy created.
                self.undo.truncate(mark);
                self.undo.push(UndoStep::Moved { from: source.to_path_buf(), to: target });
                Ok(())
            }
            Err(e) => Err(OpError::Io(source.into(), e.into())),
        }
    }
}

/// Extended attributes a copy keeps: user ones (our tags in user.xdg.tags) and POSIX ACLs. Security labels belong to
/// the target place, so they are not copied. A target file system without xattrs keeps the data only.
fn copy_xattrs(source: &Path, target: &Path) {
    let mut names = vec![0u8; 64 * 1024];
    let Ok(n) = rustix::fs::llistxattr(source, &mut names[..]) else {
        return;
    };
    let mut value = vec![0u8; 64 * 1024];
    for name in names[..n].split(|b| *b == 0).filter(|name| !name.is_empty()) {
        if !(name.starts_with(b"user.") || name.starts_with(b"system.posix_acl_")) {
            continue;
        }
        let name = std::ffi::OsStr::from_bytes(name);
        if let Ok(len) = rustix::fs::lgetxattr(source, name, &mut value[..]) {
            let _ = rustix::fs::lsetxattr(target, name, &value[..len], rustix::fs::XattrFlags::empty());
        }
    }
}

/// A folder's times, set after its contents went in (each new entry moved its mtime to now).
fn set_times(target: &Path, meta: &fs::Metadata) {
    let times = Timestamps {
        last_access: Timespec { tv_sec: meta.atime(), tv_nsec: meta.atime_nsec() },
        last_modification: Timespec { tv_sec: meta.mtime(), tv_nsec: meta.mtime_nsec() },
    };
    let _ = rustix::fs::utimensat(CWD, target, &times, rustix::fs::AtFlags::SYMLINK_NOFOLLOW);
}

fn same_file(a: &Path, b: &Path) -> bool {
    match (fs::symlink_metadata(a), fs::symlink_metadata(b)) {
        (Ok(a), Ok(b)) => a.dev() == b.dev() && a.ino() == b.ino(),
        _ => false,
    }
}

fn remove_all(path: &Path) -> io::Result<()> {
    if fs::symlink_metadata(path)?.is_dir() { fs::remove_dir_all(path) } else { fs::remove_file(path) }
}

/// Copies `sources` into the folder `dir`. On error or cancel, the undo steps so far are returned with it.
pub fn copy(sources: &[PathBuf], dir: &Path, ctl: &dyn Control) -> (Vec<UndoStep>, OpResult<()>) {
    let mut run = Run::new(ctl);
    let result = sources.iter().try_for_each(|source| run.copy_into(source, dir));
    (run.undo, result)
}

/// Moves `sources` into the folder `dir`.
pub fn move_to(sources: &[PathBuf], dir: &Path, ctl: &dyn Control) -> (Vec<UndoStep>, OpResult<()>) {
    let mut run = Run::new(ctl);
    let result = sources.iter().try_for_each(|source| run.move_into(source, dir));
    (run.undo, result)
}

pub fn trash_all(paths: &[PathBuf], ctl: &dyn Control) -> (Vec<UndoStep>, OpResult<()>) {
    let mut run = Run::new(ctl);
    let result = paths.iter().try_for_each(|path| {
        run.check()?;
        let (files, bytes) = measure(std::slice::from_ref(path));
        let trashed = trash::trash(path).map_err(io_at(path))?;
        run.undo.push(UndoStep::Trashed(trashed));
        run.tick(files, bytes, path);
        Ok(())
    });
    (run.undo, result)
}

/// A valid single file name: not empty, not "." or "..", no "/" or NUL.
pub fn valid_name(name: &str) -> bool {
    !name.is_empty() && name != "." && name != ".." && !name.contains(['/', '\0'])
}

/// Renames `path` in its folder. Fails, writing nothing, when the new name is taken.
pub fn rename(path: &Path, name: &str, ctl: &dyn Control) -> (Vec<UndoStep>, OpResult<()>) {
    let mut run = Run::new(ctl);
    let parent = path.parent().unwrap_or(Path::new("/"));
    let target = parent.join(name);
    let result = renameat_with(CWD, path, CWD, &target, RenameFlags::NOREPLACE).map_err(errno_at(&target));
    if result.is_ok() {
        run.undo.push(UndoStep::Moved { from: path.to_path_buf(), to: target.clone() });
        run.tick(1, 0, &target);
    }
    (run.undo, result)
}

/// Renames many items as one step (batch rename). Each item first takes a hidden temporary name, so swaps and
/// chains (a -> b, b -> c) work; a failure puts every item back and writes nothing.
pub fn rename_many(items: &[(PathBuf, String)], ctl: &dyn Control) -> (Vec<UndoStep>, OpResult<()>) {
    let mut run = Run::new(ctl);
    let temp_of = |i: usize, path: &Path| {
        path.parent().unwrap_or(Path::new("/")).join(format!(".andromeda-rename-{}-{i}", std::process::id()))
    };
    let mut parked: Vec<usize> = Vec::new();
    let mut result = Ok(());
    for (i, (path, _)) in items.iter().enumerate() {
        let temp = temp_of(i, path);
        if let Err(e) = renameat_with(CWD, path, CWD, &temp, RenameFlags::NOREPLACE) {
            result = Err(OpError::Io(path.clone(), e.into()));
            break;
        }
        parked.push(i);
    }
    let mut placed: Vec<usize> = Vec::new();
    if result.is_ok() {
        for &i in &parked {
            let (path, name) = &items[i];
            let target = path.parent().unwrap_or(Path::new("/")).join(name);
            if let Err(e) = renameat_with(CWD, temp_of(i, path), CWD, &target, RenameFlags::NOREPLACE) {
                result = Err(OpError::Io(target, e.into()));
                break;
            }
            placed.push(i);
        }
    }
    if result.is_err() {
        // Back out: placed items go back to their temporary names, then every parked item to its old name.
        for &i in placed.iter().rev() {
            let (path, name) = &items[i];
            let target = path.parent().unwrap_or(Path::new("/")).join(name);
            let _ = renameat_with(CWD, &target, CWD, temp_of(i, path), RenameFlags::NOREPLACE);
        }
        for &i in parked.iter().rev() {
            let _ = renameat_with(CWD, temp_of(i, &items[i].0), CWD, &items[i].0, RenameFlags::NOREPLACE);
        }
        return (Vec::new(), result);
    }
    for (path, name) in items {
        let target = path.parent().unwrap_or(Path::new("/")).join(name);
        run.undo.push(UndoStep::Moved { from: path.clone(), to: target.clone() });
        run.tick(1, 0, &target);
    }
    (run.undo, result)
}

/// Makes the folder `dir/name`, or `name (copy)` and so on when the name is taken.
pub fn new_folder(dir: &Path, name: &str, ctl: &dyn Control) -> (Vec<UndoStep>, OpResult<()>) {
    let mut run = Run::new(ctl);
    let mut target = dir.join(name);
    if fs::symlink_metadata(&target).is_ok() {
        target = free_name(&target);
    }
    let result = fs::create_dir(&target).map_err(io_at(&target));
    if result.is_ok() {
        run.undo.push(UndoStep::Created(target.clone()));
        run.tick(1, 0, &target);
    }
    (run.undo, result)
}

/// Puts a symbolic link to each source into `dir`, named as the source (a free name when taken).
pub fn link(sources: &[PathBuf], dir: &Path, ctl: &dyn Control) -> (Vec<UndoStep>, OpResult<()>) {
    let mut run = Run::new(ctl);
    let result = sources.iter().try_for_each(|source| {
        run.check()?;
        let name = source.file_name().ok_or_else(|| OpError::Io(source.clone(), io::ErrorKind::InvalidInput.into()))?;
        let mut target = dir.join(name);
        if fs::symlink_metadata(&target).is_ok() {
            target = free_name(&target);
        }
        symlink(source, &target).map_err(io_at(&target))?;
        run.undo.push(UndoStep::Created(target.clone()));
        run.tick(1, 0, &target);
        Ok(())
    });
    (run.undo, result)
}

/// Deletes for good: nothing to undo.
pub fn delete(paths: &[PathBuf], ctl: &dyn Control) -> OpResult<()> {
    let mut run = Run::new(ctl);
    paths.iter().try_for_each(|path| {
        run.check()?;
        let (files, bytes) = measure(std::slice::from_ref(path));
        remove_all(path).map_err(io_at(path))?;
        run.tick(files, bytes, path);
        Ok(())
    })
}

/// Runs `steps` backwards. Returns the steps that redo the undo, the result, and the steps it did not finish (the
/// failed one and those before it), so a retry can run them.
pub fn undo(steps: &[UndoStep], ctl: &dyn Control) -> (Vec<UndoStep>, OpResult<()>, Vec<UndoStep>) {
    let mut run = Run::new(ctl);
    let mut left = steps.len();
    let result = steps.iter().rev().try_for_each(|step| {
        run.check()?;
        match step {
            UndoStep::Created(path) => {
                // Only the outermost created path is trashed; paths inside it went with it.
                if fs::symlink_metadata(path).is_ok() {
                    let trashed = trash::trash(path).map_err(io_at(path))?;
                    run.undo.push(UndoStep::Trashed(trashed));
                }
            }
            UndoStep::Moved { from, to } => {
                let parent = from.parent().unwrap_or(Path::new("/"));
                fs::create_dir_all(parent).map_err(io_at(parent))?;
                run.move_back(to, from)?;
            }
            UndoStep::Trashed(trashed) => {
                let restored = trash::restore(trashed).map_err(io_at(&trashed.original))?;
                run.undo.push(UndoStep::Created(restored));
            }
            UndoStep::Tags { path, raw } => {
                let now = crate::tags::read_raw(path).map_err(io_at(path))?;
                crate::tags::write_raw(path, raw.as_deref()).map_err(io_at(path))?;
                run.undo.push(UndoStep::Tags { path: path.clone(), raw: now });
            }
            UndoStep::Mode { path, mode } => {
                let meta = fs::symlink_metadata(path).map_err(io_at(path))?;
                if !meta.is_symlink() {
                    fs::set_permissions(path, fs::Permissions::from_mode(*mode)).map_err(io_at(path))?;
                    run.undo.push(UndoStep::Mode { path: path.clone(), mode: meta.mode() & 0o7777 });
                }
            }
        }
        run.tick(1, 0, Path::new(""));
        left -= 1;
        Ok(())
    });
    (run.undo, result, steps[..left].to_vec())
}

impl Run<'_> {
    /// Moves `source` to the exact path `target`; a new name when something sits there now.
    fn move_back(&mut self, source: &Path, target: &Path) -> OpResult<()> {
        let target = if fs::symlink_metadata(target).is_ok() { free_name(target) } else { target.to_path_buf() };
        match renameat_with(CWD, source, CWD, &target, RenameFlags::NOREPLACE) {
            Ok(()) => {}
            Err(Errno::XDEV) => {
                let mark = self.undo.len();
                self.copy_to(source, &target, false, true)?;
                remove_all(source).map_err(io_at(source))?;
                self.undo.truncate(mark);
            }
            Err(e) => return Err(OpError::Io(source.into(), e.into())),
        }
        self.undo.push(UndoStep::Moved { from: source.to_path_buf(), to: target });
        Ok(())
    }
}

#[cfg(test)]
#[allow(clippy::expect_used)]
mod tests {
    use std::cell::RefCell;
    use std::sync::LazyLock;

    use super::*;

    static TRASH: LazyLock<tempfile::TempDir> = LazyLock::new(|| {
        let dir = tempfile::tempdir().expect("temp dir");
        trash::use_home_trash(dir.path().join("Trash"));
        dir
    });

    struct Answers {
        answers: RefCell<Vec<(Resolution, bool)>>,
        asked: RefCell<Vec<PathBuf>>,
    }

    impl Answers {
        fn new(answers: &[(Resolution, bool)]) -> Self {
            LazyLock::force(&TRASH);
            Self { answers: RefCell::new(answers.iter().rev().copied().collect()), asked: RefCell::new(Vec::new()) }
        }
    }

    impl Control for Answers {
        fn cancelled(&self) -> bool {
            false
        }
        fn progress(&self, _: u64, _: u64, _: &Path) {}
        fn conflict(&self, _: &Path, target: &Path) -> Option<(Resolution, bool)> {
            self.asked.borrow_mut().push(target.to_path_buf());
            self.answers.borrow_mut().pop()
        }
    }

    fn write(path: &Path, text: &str) {
        fs::create_dir_all(path.parent().expect("parent")).expect("mkdir");
        fs::write(path, text).expect("write");
    }

    fn read(path: &Path) -> String {
        fs::read_to_string(path).unwrap_or_default()
    }

    #[test]
    fn copy_keep_both_names_the_copy_and_undo_removes_it() {
        let root = tempfile::tempdir().expect("tmp");
        let (src, dst) = (root.path().join("src"), root.path().join("dst"));
        write(&src.join("a.txt"), "new");
        write(&dst.join("a.txt"), "old");
        let ctl = Answers::new(&[(Resolution::KeepBoth, false)]);
        let (steps, result) = copy(&[src.join("a.txt")], &dst, &ctl);
        assert!(result.is_ok());
        assert_eq!(*ctl.asked.borrow(), vec![dst.join("a.txt")]);
        assert_eq!((read(&dst.join("a.txt")), read(&dst.join("a (copy).txt"))), ("old".into(), "new".into()));
        let (_, result, _) = undo(&steps, &ctl);
        assert!(result.is_ok());
        assert!(!dst.join("a (copy).txt").exists());
        assert_eq!(read(&dst.join("a.txt")), "old");
    }

    #[test]
    fn copy_keeps_tags_holes_and_folder_mode() {
        let root = tempfile::tempdir().expect("tmp");
        let (src, dst) = (root.path().join("src"), root.path().join("dst"));
        let dir = src.join("private");
        write(&dir.join("disk.img"), "");
        let file = OpenOptions::new().write(true).open(dir.join("disk.img")).expect("open");
        file.write_all_at(b"head", 0).expect("head");
        file.write_all_at(b"tail", 64 << 20).expect("tail");
        drop(file);
        rustix::fs::setxattr(dir.join("disk.img"), "user.xdg.tags", b"Work", rustix::fs::XattrFlags::empty())
            .expect("tag (needs a file system with user xattrs)");
        fs::set_permissions(&dir, fs::Permissions::from_mode(0o700)).expect("chmod");
        fs::create_dir_all(&dst).expect("dst");
        let ctl = Answers::new(&[]);
        let (_, result) = copy(&[dir.clone()], &dst, &ctl);
        assert!(result.is_ok());
        let copied = dst.join("private/disk.img");
        let meta = fs::metadata(&copied).expect("copied");
        assert_eq!(meta.len(), (64 << 20) + 4);
        assert!(meta.blocks() * 512 < 1 << 20, "the hole was filled: {} blocks", meta.blocks());
        let mut tag = [0u8; 16];
        let n = rustix::fs::getxattr(&copied, "user.xdg.tags", &mut tag[..]).expect("tag copied");
        assert_eq!(&tag[..n], b"Work");
        assert_eq!(fs::metadata(dst.join("private")).expect("dir").permissions().mode() & 0o777, 0o700);
    }

    #[test]
    fn replace_trashes_the_old_file_and_undo_brings_it_back() {
        let root = tempfile::tempdir().expect("tmp");
        let (src, dst) = (root.path().join("src"), root.path().join("dst"));
        write(&src.join("b.txt"), "new");
        write(&dst.join("b.txt"), "old");
        let ctl = Answers::new(&[(Resolution::Replace, false)]);
        let (steps, result) = copy(&[src.join("b.txt")], &dst, &ctl);
        assert!(result.is_ok());
        assert_eq!(read(&dst.join("b.txt")), "new");
        let (_, result, _) = undo(&steps, &ctl);
        assert!(result.is_ok());
        assert_eq!(read(&dst.join("b.txt")), "old");
    }

    #[test]
    fn folder_merge_merges_and_apply_to_all_asks_once() {
        let root = tempfile::tempdir().expect("tmp");
        let (src, dst) = (root.path().join("src"), root.path().join("dst"));
        write(&src.join("d/x"), "x2");
        write(&src.join("d/y"), "y2");
        write(&src.join("d/z"), "z");
        write(&dst.join("d/x"), "x1");
        write(&dst.join("d/y"), "y1");
        let ctl = Answers::new(&[(Resolution::Merge, false), (Resolution::Skip, true)]);
        let (_, result) = copy(&[src.join("d")], &dst, &ctl);
        assert!(result.is_ok());
        assert_eq!(ctl.asked.borrow().len(), 2);
        assert_eq!(read(&dst.join("d/z")), "z");
        let skipped = [read(&dst.join("d/x")), read(&dst.join("d/y"))];
        assert_eq!(skipped, ["x1".to_owned(), "y1".to_owned()]);
    }

    #[test]
    fn move_then_undo_returns_the_folder() {
        let root = tempfile::tempdir().expect("tmp");
        let (src, dst) = (root.path().join("src"), root.path().join("dst"));
        write(&src.join("m/inner.txt"), "i");
        fs::create_dir_all(&dst).expect("mkdir");
        let ctl = Answers::new(&[]);
        let (steps, result) = move_to(&[src.join("m")], &dst, &ctl);
        assert!(result.is_ok());
        assert!(!src.join("m").exists());
        assert_eq!(read(&dst.join("m/inner.txt")), "i");
        let (_, result, _) = undo(&steps, &ctl);
        assert!(result.is_ok());
        assert_eq!(read(&src.join("m/inner.txt")), "i");
        assert!(!dst.join("m").exists());
    }

    #[test]
    fn trash_writes_the_spec_info_and_restore_puts_it_back() {
        let root = tempfile::tempdir().expect("tmp");
        let file = root.path().join("dir with space/t.txt");
        write(&file, "t");
        let ctl = Answers::new(&[]);
        let (steps, result) = trash_all(std::slice::from_ref(&file), &ctl);
        assert!(result.is_ok());
        assert!(!file.exists());
        let UndoStep::Trashed(entry) = &steps[0] else { panic!("not trashed") };
        let info = read(&entry.info());
        assert!(info.starts_with("[Trash Info]\nPath="));
        assert!(info.contains("dir%20with%20space/t.txt"));
        assert!(info.contains("\nDeletionDate="));
        let parsed = trash::read_info(&entry.trash_dir, &entry.name).expect("info");
        assert_eq!(parsed.original, file);
        let (_, result, _) = undo(&steps, &ctl);
        assert!(result.is_ok());
        assert_eq!(read(&file), "t");
        assert!(!entry.info().exists());
    }

    #[test]
    fn move_into_its_own_folder_changes_nothing() {
        let root = tempfile::tempdir().expect("tmp");
        write(&root.path().join("f"), "f");
        let ctl = Answers::new(&[]);
        let (steps, result) = move_to(&[root.path().join("f")], root.path(), &ctl);
        assert!(result.is_ok() && steps.is_empty());
        assert_eq!(read(&root.path().join("f")), "f");
        assert!(!root.path().join("f (copy)").exists());
    }

    #[test]
    fn copy_of_a_folder_into_itself_fails_without_writing() {
        let root = tempfile::tempdir().expect("tmp");
        let dir = root.path().join("a");
        write(&dir.join("sub/x"), "x");
        let ctl = Answers::new(&[]);
        let (steps, result) = copy(std::slice::from_ref(&dir), &dir.join("sub"), &ctl);
        assert!(result.is_err() && steps.is_empty());
        assert!(!dir.join("sub/a").exists());
    }

    #[test]
    fn rename_many_swaps_names_and_backs_out_on_a_taken_name() {
        let dir = tempfile::tempdir().expect("temp dir");
        let p = |n: &str| dir.path().join(n);
        write(&p("a.txt"), "a");
        write(&p("b.txt"), "b");
        write(&p("c.txt"), "c");
        let ctl = Answers::new(&[]);
        let (steps, ok) = rename_many(&[(p("a.txt"), "b.txt".into()), (p("b.txt"), "a.txt".into())], &ctl);
        assert!(ok.is_ok());
        assert_eq!((read(&p("a.txt")), read(&p("b.txt"))), ("b".to_string(), "a".to_string()));
        assert_eq!(steps.len(), 2);
        let (_, taken) = rename_many(&[(p("a.txt"), "x.txt".into()), (p("b.txt"), "c.txt".into())], &ctl);
        assert!(taken.is_err());
        assert_eq!(read(&p("a.txt")), "b");
        assert_eq!(read(&p("b.txt")), "a");
        assert!(!p("x.txt").exists());
    }

    #[test]
    fn rename_refuses_a_taken_name_and_undo_puts_the_old_name_back() {
        let dir = tempfile::tempdir().expect("temp dir");
        write(&dir.path().join("a.txt"), "a");
        write(&dir.path().join("b.txt"), "b");
        let ctl = Answers::new(&[]);
        let (_, taken) = rename(&dir.path().join("a.txt"), "b.txt", &ctl);
        assert!(taken.is_err());
        assert_eq!(read(&dir.path().join("b.txt")), "b");
        let (steps, ok) = rename(&dir.path().join("a.txt"), "c.txt", &ctl);
        assert!(ok.is_ok());
        assert_eq!(read(&dir.path().join("c.txt")), "a");
        let (_, undone, _) = undo(&steps, &ctl);
        assert!(undone.is_ok());
        assert_eq!(read(&dir.path().join("a.txt")), "a");
        assert!(!dir.path().join("c.txt").exists());
    }

    #[test]
    fn new_folder_takes_a_free_name_and_link_points_at_the_source() {
        let dir = tempfile::tempdir().expect("temp dir");
        let ctl = Answers::new(&[]);
        assert!(new_folder(dir.path(), "untitled folder", &ctl).1.is_ok());
        assert!(new_folder(dir.path(), "untitled folder", &ctl).1.is_ok());
        assert!(dir.path().join("untitled folder (copy)").is_dir());
        write(&dir.path().join("a.txt"), "a");
        let (steps, ok) = link(&[dir.path().join("a.txt")], &dir.path().join("untitled folder"), &ctl);
        assert!(ok.is_ok());
        assert_eq!(read(&dir.path().join("untitled folder/a.txt")), "a");
        let _ = &*TRASH;
        assert!(undo(&steps, &ctl).1.is_ok());
        assert!(fs::symlink_metadata(dir.path().join("untitled folder/a.txt")).is_err());
    }

    #[test]
    fn redo_of_an_undone_trash_trashes_again() {
        let root = tempfile::tempdir().expect("tmp");
        let file = root.path().join("r.txt");
        write(&file, "r");
        let ctl = Answers::new(&[]);
        let (steps, _) = trash_all(std::slice::from_ref(&file), &ctl);
        let (redo, result, left) = undo(&steps, &ctl);
        assert!(result.is_ok() && left.is_empty() && file.exists());
        let (_, result, _) = undo(&redo, &ctl);
        assert!(result.is_ok());
        assert!(!file.exists());
        assert!(!root.path().join("r (copy).txt").exists());
    }

    #[test]
    fn folder_replace_swaps_the_whole_folder_and_undo_brings_it_back() {
        let root = tempfile::tempdir().expect("tmp");
        let (src, dst) = (root.path().join("src"), root.path().join("dst"));
        write(&src.join("d/x"), "x2");
        write(&dst.join("d/x"), "x1");
        write(&dst.join("d/old"), "o");
        let ctl = Answers::new(&[(Resolution::Replace, false)]);
        let (steps, result) = copy(&[src.join("d")], &dst, &ctl);
        assert!(result.is_ok());
        assert_eq!(ctl.asked.borrow().len(), 1, "a replaced folder asks nothing inside");
        assert_eq!(read(&dst.join("d/x")), "x2");
        assert!(!dst.join("d/old").exists());
        let (_, result, _) = undo(&steps, &ctl);
        assert!(result.is_ok());
        assert_eq!(read(&dst.join("d/old")), "o");
    }

    #[test]
    fn count_conflicts_matches_the_questions_a_merge_asks() {
        let root = tempfile::tempdir().expect("tmp");
        let (src, dst) = (root.path().join("src"), root.path().join("dst"));
        write(&src.join("d/x"), "x2");
        write(&src.join("d/sub/y"), "y2");
        write(&src.join("d/new"), "n");
        write(&src.join("top.txt"), "t2");
        write(&dst.join("d/x"), "x1");
        write(&dst.join("d/sub/y"), "y1");
        write(&dst.join("top.txt"), "t1");
        let sources = [src.join("d"), src.join("top.txt")];
        let total = count_conflicts(&sources, &dst).expect("counted");
        let ctl = Answers::new(&[(Resolution::Merge, false); 8]);
        let (_, result) = copy(&sources, &dst, &ctl);
        assert!(result.is_ok());
        // d, d/x, d/sub, d/sub/y, top.txt
        assert_eq!(ctl.asked.borrow().len() as u64, total);
        assert_eq!(total, 5);
    }

    #[test]
    fn folder_clash_counts_new_same_and_different_with_hints() {
        let root = tempfile::tempdir().expect("tmp");
        let (src, dst) = (root.path().join("src/d"), root.path().join("dst/d"));
        write(&src.join("same.txt"), "same");
        write(&dst.join("same.txt"), "same");
        let when = fs::metadata(src.join("same.txt")).expect("meta").modified().expect("mtime");
        File::options().write(true).open(dst.join("same.txt")).expect("open").set_modified(when).expect("touch");
        write(&src.join("big.txt"), "larger here");
        write(&dst.join("big.txt"), "small");
        File::options()
            .write(true)
            .open(dst.join("big.txt"))
            .expect("open")
            .set_modified(when - std::time::Duration::from_secs(60))
            .expect("touch");
        File::options().write(true).open(src.join("big.txt")).expect("open").set_modified(when).expect("touch");
        write(&src.join("only/a"), "a");
        write(&src.join("only/b"), "b");
        let clash = folder_clash(&src, &dst).expect("both folders");
        assert_eq!((clash.new, clash.same, clash.different), (2, 1, 1));
        assert_eq!(clash.names, vec![(PathBuf::from("big.txt"), "newer here, larger here".to_string())]);
        assert!(folder_clash(&src.join("same.txt"), &dst).is_none());
    }
}
