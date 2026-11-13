//! Storage scan (Files "Storage" page): bytes on disk per category, the largest folders, and clean-up candidates.
//! Runs as a job on a blocking worker; the folder sizes stay cached so drill-down needs no second walk.

use std::collections::{HashMap, HashSet};
use std::fs;
use std::os::unix::fs::MetadataExt;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Condvar, LazyLock, Mutex, PoisonError};
use std::time::{Duration, SystemTime, UNIX_EPOCH};

use serde::{Deserialize, Serialize};

use crate::ops::{Control, OpError, OpResult};

/// A scan younger than this is reused.
const CACHE_AGE: Duration = Duration::from_secs(600);
const LARGEST: usize = 10;
/// A folder whose biggest subfolder holds this share of it is shown as that subfolder (Steam, not .local).
const DOMINANT: f64 = 0.6;
const OLD_DOWNLOAD: Duration = Duration::from_secs(90 * 24 * 3600);
const MAX_OLD_ITEMS: usize = 500;
/// How often the scan reports progress and looks for a cancel.
const PROGRESS_TICK: Duration = Duration::from_millis(100);

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum Category {
    Apps,
    Documents,
    Pictures,
    Videos,
    Music,
    Archives,
    Caches,
    Other,
}

impl Category {
    pub const ALL: [Self; 8] = [
        Self::Apps,
        Self::Documents,
        Self::Pictures,
        Self::Videos,
        Self::Music,
        Self::Archives,
        Self::Caches,
        Self::Other,
    ];

    pub fn name(self) -> &'static str {
        match self {
            Self::Apps => "Apps and system",
            Self::Documents => "Documents",
            Self::Pictures => "Pictures",
            Self::Videos => "Videos",
            Self::Music => "Music",
            Self::Archives => "Archives",
            Self::Caches => "Caches",
            Self::Other => "Other",
        }
    }
}

/// The category of one file: caches and app data by place, the rest by extension.
pub fn category(path: &Path, home: &Path) -> Category {
    if let Ok(rel) = path.strip_prefix(home) {
        let first = rel.components().next().map(|c| c.as_os_str().to_string_lossy().into_owned()).unwrap_or_default();
        match first.as_str() {
            ".cache" => return Category::Caches,
            ".local" | ".var" | ".steam" | ".cargo" | ".rustup" | ".npm" | ".config" => return Category::Apps,
            _ => {}
        }
    } else {
        return Category::Apps;
    }
    let ext = path.extension().map(|e| e.to_string_lossy().to_ascii_lowercase()).unwrap_or_default();
    match ext.as_str() {
        "pdf" | "doc" | "docx" | "odt" | "ods" | "odp" | "xls" | "xlsx" | "ppt" | "pptx" | "txt" | "md" | "rtf"
        | "epub" | "csv" | "tex" => Category::Documents,
        "jpg" | "jpeg" | "png" | "gif" | "webp" | "heic" | "heif" | "avif" | "jxl" | "tif" | "tiff" | "bmp" | "svg"
        | "raw" | "cr2" | "nef" | "arw" | "dng" => Category::Pictures,
        "mp4" | "mkv" | "mov" | "webm" | "avi" | "m4v" | "wmv" | "flv" | "mpg" | "mpeg" => Category::Videos,
        "mp3" | "flac" | "ogg" | "opus" | "m4a" | "wav" | "aac" | "wma" | "aiff" => Category::Music,
        "zip" | "7z" | "tar" | "gz" | "xz" | "zst" | "bz2" | "rar" | "iso" | "img" | "deb" | "rpm" => {
            Category::Archives
        }
        _ => Category::Other,
    }
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct Summary {
    pub root: PathBuf,
    pub total: u64,
    /// (category name, bytes), largest first, empty ones left out.
    pub categories: Vec<(String, u64)>,
    pub largest: Vec<(PathBuf, u64)>,
    pub trash: u64,
    pub caches: u64,
    pub old_downloads: u64,
    /// Downloads not changed for 90 days, largest first (at most 500).
    pub old_download_items: Vec<(PathBuf, u64)>,
    pub scanned_at: u64,
    /// Read back from the disk cache of an earlier daemon run: the page shows it and asks for a fresh scan.
    #[serde(default)]
    pub cached: bool,
}

struct Cache {
    at: SystemTime,
    summary: Summary,
    /// Every folder under the root with the bytes under it.
    dirs: HashMap<PathBuf, u64>,
}

static CACHE: LazyLock<Mutex<HashMap<PathBuf, Cache>>> = LazyLock::new(|| Mutex::new(HashMap::new()));

fn home() -> PathBuf {
    std::env::var_os("HOME").map_or_else(|| PathBuf::from("/"), PathBuf::from)
}

/// Bytes on disk (blocks), so sparse files count what they use.
fn on_disk(meta: &fs::Metadata) -> u64 {
    meta.blocks().saturating_mul(512)
}

/// Walks `root` on one file system; hard links count once. `fresh`: walk even when a recent scan exists.
pub fn scan(root: &Path, fresh: bool, ctl: &dyn Control) -> OpResult<Summary> {
    if !fresh
        && let Some(cache) = CACHE.lock().unwrap_or_else(PoisonError::into_inner).get(root)
        && cache.at.elapsed().is_ok_and(|age| age < CACHE_AGE)
    {
        return Ok(cache.summary.clone());
    }
    // After a restart the last scan from disk answers at once; it is older than CACHE_AGE, so the page asks for a
    // fresh walk next (Summary::scanned_at).
    if !fresh
        && !CACHE.lock().unwrap_or_else(PoisonError::into_inner).contains_key(root)
        && let Some(saved) = load(root)
    {
        return Ok(saved);
    }
    scan_rooted(root, &home(), ctl)
}

/// The walk behind `scan`, with the home folder that decides caches, app data, Trash and Downloads.
/// One walker's counts; the walkers' parts add up after the walk.
#[derive(Default)]
struct Part {
    /// Bytes of the files directly in each folder (every folder walked has an entry).
    direct: HashMap<PathBuf, u64>,
    per_category: HashMap<Category, u64>,
    old_items: Vec<(PathBuf, u64)>,
    total: u64,
    trash: u64,
    caches: u64,
    old: u64,
    files: u64,
}

/// Folders waiting to be read, and how many walkers are reading one now.
struct Queue {
    todo: Vec<PathBuf>,
    busy: usize,
}

struct Walk<'a> {
    root: &'a Path,
    home: &'a Path,
    device: u64,
    now: SystemTime,
    downloads: PathBuf,
    trash: PathBuf,
    caches: PathBuf,
    queue: Mutex<Queue>,
    wake: Condvar,
    stop: AtomicBool,
    files: AtomicU64,
    bytes: AtomicU64,
    /// Inodes with more than one link, so a hard link counts once.
    seen: Mutex<HashSet<(u64, u64)>>,
}

impl Walk<'_> {
    fn next(&self) -> Option<PathBuf> {
        let mut q = self.queue.lock().unwrap_or_else(PoisonError::into_inner);
        loop {
            if self.stop.load(Ordering::Relaxed) {
                return None;
            }
            if let Some(dir) = q.todo.pop() {
                q.busy += 1;
                return Some(dir);
            }
            if q.busy == 0 {
                return None;
            }
            q = self.wake.wait(q).unwrap_or_else(PoisonError::into_inner);
        }
    }

    fn done(&self, found: Vec<PathBuf>) {
        let mut q = self.queue.lock().unwrap_or_else(PoisonError::into_inner);
        q.busy -= 1;
        let last = q.busy == 0 && q.todo.is_empty() && found.is_empty();
        let more = found.len();
        q.todo.extend(found);
        drop(q);
        // New folders wake that many idle walkers; the last folder read wakes them all to finish.
        if last || more > 1 {
            self.wake.notify_all();
        } else if more == 1 {
            self.wake.notify_one();
        }
    }

    fn walker(&self) -> Part {
        let mut part = Part::default();
        let mut counted = 0u64;
        while let Some(dir) = self.next() {
            let mut found = Vec::new();
            let mut here = 0u64;
            if let Ok(entries) = fs::read_dir(&dir) {
                for entry in entries.flatten() {
                    // DirEntry::metadata does not follow links (fstatat on the open folder).
                    let Ok(meta) = entry.metadata() else { continue };
                    if meta.dev() != self.device {
                        continue;
                    }
                    if meta.is_dir() {
                        found.push(entry.path());
                        continue;
                    }
                    if meta.nlink() > 1
                        && !self.seen.lock().unwrap_or_else(PoisonError::into_inner).insert((meta.dev(), meta.ino()))
                    {
                        continue;
                    }
                    let bytes = on_disk(&meta);
                    let path = entry.path();
                    here += bytes;
                    part.files += 1;
                    *part.per_category.entry(category(&path, self.home)).or_default() += bytes;
                    if path.starts_with(&self.trash) {
                        part.trash += bytes;
                    } else if path.starts_with(&self.caches) {
                        part.caches += bytes;
                    } else if path.starts_with(&self.downloads)
                        && meta.modified().is_ok_and(|m| self.now.duration_since(m).is_ok_and(|age| age > OLD_DOWNLOAD))
                    {
                        part.old += bytes;
                        part.old_items.push((path, bytes));
                    }
                }
            }
            part.total += here;
            self.files.fetch_add(part.files - counted, Ordering::Relaxed);
            counted = part.files;
            self.bytes.fetch_add(here, Ordering::Relaxed);
            part.direct.insert(dir, here);
            self.done(found);
        }
        part
    }
}

/// The walk behind `scan`, with the home folder that decides caches, app data, Trash and Downloads. Folders are read
/// by one walker per core; each folder's size is summed once from its files and subfolders after the walk.
fn scan_rooted(root: &Path, home: &Path, ctl: &dyn Control) -> OpResult<Summary> {
    let root_meta = fs::symlink_metadata(root).map_err(|e| OpError::Io(root.to_path_buf(), e))?;
    let walk = Walk {
        root,
        home,
        device: root_meta.dev(),
        now: SystemTime::now(),
        downloads: home.join("Downloads"),
        trash: home.join(".local/share/Trash"),
        caches: home.join(".cache"),
        queue: Mutex::new(Queue { todo: vec![root.to_path_buf()], busy: 0 }),
        wake: Condvar::new(),
        stop: AtomicBool::new(false),
        files: AtomicU64::new(0),
        bytes: AtomicU64::new(0),
        seen: Mutex::new(HashSet::new()),
    };
    let walkers = std::thread::available_parallelism().map_or(4, |n| n.get().clamp(2, 16));
    let parts: Vec<Part> = std::thread::scope(|scope| {
        let handles: Vec<_> = (0..walkers).map(|_| scope.spawn(|| walk.walker())).collect();
        // Control is not shared with the walkers: this thread reports progress and passes on a cancel.
        while handles.iter().any(|h| !h.is_finished()) {
            if ctl.cancelled() {
                walk.stop.store(true, Ordering::Relaxed);
                walk.wake.notify_all();
            }
            ctl.progress(walk.files.load(Ordering::Relaxed), walk.bytes.load(Ordering::Relaxed), walk.root);
            std::thread::sleep(PROGRESS_TICK);
        }
        handles.into_iter().filter_map(|h| h.join().ok()).collect()
    });
    if walk.stop.load(Ordering::Relaxed) {
        return Err(OpError::Cancelled);
    }

    let mut dirs: HashMap<PathBuf, u64> = HashMap::new();
    let mut per_category: HashMap<Category, u64> = HashMap::new();
    let mut old_items = Vec::new();
    let (mut total, mut trash_bytes, mut cache_bytes, mut old_bytes, mut files) = (0u64, 0u64, 0u64, 0u64, 0u64);
    for part in parts {
        dirs.extend(part.direct);
        for (category, bytes) in part.per_category {
            *per_category.entry(category).or_default() += bytes;
        }
        old_items.extend(part.old_items);
        total += part.total;
        trash_bytes += part.trash;
        cache_bytes += part.caches;
        old_bytes += part.old;
        files += part.files;
    }
    // Deepest folders first, each adds its sum to its parent: every folder then holds all bytes under it.
    let mut order: Vec<PathBuf> = dirs.keys().filter(|p| p.as_path() != root).cloned().collect();
    order.sort_by_key(|p| std::cmp::Reverse(p.components().count()));
    for dir in order {
        let size = dirs.get(&dir).copied().unwrap_or(0);
        if let Some(parent) = dir.parent().and_then(|p| dirs.get_mut(p)) {
            *parent += size;
        }
    }

    let mut categories: Vec<(String, u64)> = Category::ALL
        .iter()
        .filter_map(|c| per_category.get(c).filter(|&&b| b > 0).map(|&b| (c.name().to_owned(), b)))
        .collect();
    categories.sort_by_key(|item| std::cmp::Reverse(item.1));
    old_items.sort_by_key(|item| std::cmp::Reverse(item.1));
    old_items.truncate(MAX_OLD_ITEMS);
    let summary = Summary {
        root: root.to_path_buf(),
        total,
        categories,
        largest: largest(root, &dirs),
        trash: trash_bytes,
        caches: cache_bytes,
        old_downloads: old_bytes,
        old_download_items: old_items,
        scanned_at: walk.now.duration_since(UNIX_EPOCH).map_or(0, |d| d.as_secs()),
        cached: false,
    };
    ctl.progress(files, total, root);
    save(&summary);
    CACHE
        .lock()
        .unwrap_or_else(PoisonError::into_inner)
        .insert(root.to_path_buf(), Cache { at: walk.now, summary: summary.clone(), dirs });
    Ok(summary)
}

/// The last scan of `root` on disk ($XDG_CACHE_HOME/andromeda/storage), so the page shows numbers at once after a
/// restart while a fresh walk runs.
fn saved_path(root: &Path) -> PathBuf {
    let base = std::env::var_os("XDG_CACHE_HOME").map_or_else(|| home().join(".cache"), PathBuf::from);
    let name: String =
        root.to_string_lossy().chars().map(|c| if c.is_ascii_alphanumeric() { c } else { '_' }).collect();
    base.join("andromeda/storage").join(format!("{name}.json"))
}

fn save(summary: &Summary) {
    let path = saved_path(&summary.root);
    let Ok(json) = serde_json::to_vec(summary) else { return };
    let tmp = path.with_extension("json.tmp");
    let written = path.parent().is_some_and(|dir| fs::create_dir_all(dir).is_ok())
        && fs::write(&tmp, json).is_ok()
        && fs::rename(&tmp, &path).is_ok();
    if !written {
        log::warn!("storage: cannot keep the scan of {} in {}", summary.root.display(), path.display());
    }
}

fn load(root: &Path) -> Option<Summary> {
    let mut summary: Summary = serde_json::from_slice(&fs::read(saved_path(root)).ok()?).ok()?;
    summary.cached = true;
    (summary.root == root).then_some(summary)
}

fn children<'a>(dir: &Path, dirs: &'a HashMap<PathBuf, u64>) -> Vec<(&'a PathBuf, u64)> {
    let mut out: Vec<_> = dirs.iter().filter(|(p, _)| p.parent() == Some(dir)).map(|(p, &s)| (p, s)).collect();
    out.sort_by(|a, b| b.1.cmp(&a.1).then_with(|| a.0.cmp(b.0)));
    out
}

/// One folder per top-level child: the child, or the subfolder that holds most of it.
fn largest(root: &Path, dirs: &HashMap<PathBuf, u64>) -> Vec<(PathBuf, u64)> {
    let mut by_parent: HashMap<&Path, Vec<(&PathBuf, u64)>> = HashMap::new();
    for (path, &size) in dirs {
        if let Some(parent) = path.parent() {
            by_parent.entry(parent).or_default().push((path, size));
        }
    }
    let mut out = Vec::new();
    for (child, size) in children(root, dirs) {
        let (mut at, mut at_size) = (child.clone(), size);
        while let Some((big, big_size)) =
            by_parent.get(at.as_path()).and_then(|kids| kids.iter().max_by_key(|k| k.1)).copied()
        {
            #[allow(clippy::cast_precision_loss)]
            if at_size == 0 || (big_size as f64) < DOMINANT * at_size as f64 {
                break;
            }
            (at, at_size) = (big.clone(), big_size);
        }
        if at_size > 0 {
            out.push((at, at_size));
        }
    }
    out.sort_by_key(|item| std::cmp::Reverse(item.1));
    out.truncate(LARGEST);
    out
}

/// Folders and files in `dir` with their bytes, largest first: folder sizes from the cached scan that covers `dir`.
pub fn folder(dir: &Path) -> std::io::Result<Vec<(PathBuf, u64, bool)>> {
    let cache = CACHE.lock().unwrap_or_else(PoisonError::into_inner);
    let dirs = cache
        .values()
        .find(|c| dir.starts_with(&c.summary.root))
        .map(|c| &c.dirs)
        .ok_or_else(|| std::io::Error::other("no storage scan covers this folder"))?;
    let mut out: Vec<(PathBuf, u64, bool)> =
        children(dir, dirs).into_iter().map(|(p, s)| (p.clone(), s, true)).collect();
    drop(cache);
    for entry in fs::read_dir(dir)?.flatten() {
        if let Ok(meta) = entry.metadata()
            && !meta.is_dir()
        {
            out.push((entry.path(), on_disk(&meta), false));
        }
    }
    out.sort_by_key(|item| std::cmp::Reverse(item.1));
    Ok(out)
}

#[cfg(test)]
#[allow(clippy::expect_used)]
mod tests {
    use super::*;
    use crate::ops::Resolution;

    struct Quietly;

    impl Control for Quietly {
        fn cancelled(&self) -> bool {
            false
        }
        fn progress(&self, _: u64, _: u64, _: &Path) {}
        fn conflict(&self, _: &Path, _: &Path) -> Option<(Resolution, bool)> {
            None
        }
    }

    fn bytes_of(summary: &Summary, name: &str) -> u64 {
        summary.categories.iter().find(|(n, _)| n == name).map_or(0, |(_, b)| *b)
    }

    #[test]
    fn totals_per_category_add_up_and_follow_kind_and_place() {
        let home = tempfile::tempdir().expect("tmp");
        let h = home.path();
        let write = |rel: &str, n: usize| {
            let p = h.join(rel);
            fs::create_dir_all(p.parent().expect("parent")).expect("mkdir");
            fs::write(&p, vec![7u8; n]).expect("write");
            on_disk(&fs::metadata(&p).expect("stat"))
        };
        let doc = write("Documents/report.pdf", 10_000) + write("notes.md", 5_000);
        let pic = write("Pictures/a.JPG", 20_000);
        let cache = write(".cache/thumbs/x.png", 30_000);
        let other = write("misc/blob.bin", 8_000);
        fs::hard_link(h.join("misc/blob.bin"), h.join("misc/blob-link.bin")).expect("hard link");
        let summary = scan_with_home(h, &Quietly);
        assert_eq!(bytes_of(&summary, "Documents"), doc);
        assert_eq!(bytes_of(&summary, "Pictures"), pic, "a .JPG is a picture");
        assert_eq!(bytes_of(&summary, "Caches"), cache, "a .png under ~/.cache is a cache, not a picture");
        assert_eq!(bytes_of(&summary, "Other"), other, "a hard link counts once");
        assert_eq!(summary.total, doc + pic + cache + other);
        assert_eq!(summary.caches, cache);
        assert_eq!(summary.categories.first().map(|(n, _)| n.as_str()), Some("Caches"), "largest first");
    }

    #[test]
    fn largest_folders_descend_into_a_dominant_subfolder() {
        let mut dirs = HashMap::new();
        let r = PathBuf::from("/h");
        for (p, s) in
            [("/h", 100), ("/h/.local", 70), ("/h/.local/share", 65), ("/h/.local/share/Steam", 60), ("/h/Videos", 30)]
        {
            dirs.insert(PathBuf::from(p), s);
        }
        let out = largest(&r, &dirs);
        assert_eq!(out, vec![(PathBuf::from("/h/.local/share/Steam"), 60), (PathBuf::from("/h/Videos"), 30)]);
    }

    fn scan_with_home(home: &Path, ctl: &dyn Control) -> Summary {
        scan_rooted(home, home, ctl).expect("scan")
    }
}
