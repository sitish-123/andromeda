//! File operations without D-Bus: copy, move, trash, restore and delete, with conflict questions, progress,
//! cancellation and the steps that undo them. Every function blocks; the bus layer runs each job on its own thread.

use std::fs::{self, File, OpenOptions};
use std::io;
use std::os::unix::fs::{MetadataExt, OpenOptionsExt, PermissionsExt, symlink};
use std::path::{Path, PathBuf};

use rustix::fs::{CWD, RenameFlags, Timespec, Timestamps, copy_file_range, futimens, ioctl_ficlone, renameat_with};
use rustix::io::Errno;

use crate::trash::{self, Trashed};

const CHUNK: usize = 8 << 20;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Resolution {
    /// Files: the old target goes to the trash (undo brings it back). Folders: merge.
    Replace,
    Skip,
    KeepBoth,
}

impl Resolution {
    pub fn parse(s: &str) -> Option<Self> {
        match s {
            "replace" => Some(Self::Replace),
            "skip" => Some(Self::Skip),
            "keep_both" => Some(Self::KeepBoth),
            _ => None,
        }
    }
}

/// What a running operation needs from its job: progress, cancellation and conflict answers.
pub trait Control {
    fn cancelled(&self) -> bool;
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
    /// Replace merges into it.
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
            Resolution::Replace => {
                let source_is_dir = fs::symlink_metadata(source).is_ok_and(|m| m.is_dir());
                if source_is_dir && existing.is_dir() {
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
                fs::create_dir(target).map_err(io_at(target))?;
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
            fs::set_permissions(target, meta.permissions()).map_err(io_at(target))?;
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
        let result = self.copy_data(&input, &output, meta.len(), source);
        if result.is_err() {
            let _ = fs::remove_file(target);
            return result;
        }
        let times = Timestamps {
            last_access: Timespec { tv_sec: meta.atime(), tv_nsec: meta.atime_nsec() },
            last_modification: Timespec { tv_sec: meta.mtime(), tv_nsec: meta.mtime_nsec() },
        };
        futimens(&output, &times).map_err(errno_at(target))
    }

    fn copy_data(&mut self, input: &File, output: &File, len: u64, source: &Path) -> OpResult<()> {
        if len > 0 && ioctl_ficlone(output, input).is_ok() {
            self.tick(0, len, source);
            return Ok(());
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
    fn folder_replace_merges_and_apply_to_all_asks_once() {
        let root = tempfile::tempdir().expect("tmp");
        let (src, dst) = (root.path().join("src"), root.path().join("dst"));
        write(&src.join("d/x"), "x2");
        write(&src.join("d/y"), "y2");
        write(&src.join("d/z"), "z");
        write(&dst.join("d/x"), "x1");
        write(&dst.join("d/y"), "y1");
        let ctl = Answers::new(&[(Resolution::Replace, false), (Resolution::Skip, true)]);
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
}
