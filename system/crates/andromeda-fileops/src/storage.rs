//! Storage scan (Files "Storage" page): bytes on disk per category, the largest folders, and clean-up candidates.
//! Runs as a job on a blocking worker; the folder sizes stay cached so drill-down needs no second walk.

use std::collections::{HashMap, HashSet};
use std::fs;
use std::os::unix::fs::MetadataExt;
use std::path::{Path, PathBuf};
use std::sync::{LazyLock, Mutex, PoisonError};
use std::time::{Duration, SystemTime, UNIX_EPOCH};

use serde::Serialize;

use crate::ops::{Control, OpError, OpResult};

/// A scan younger than this is reused.
const CACHE_AGE: Duration = Duration::from_secs(600);
const LARGEST: usize = 10;
/// A folder whose biggest subfolder holds this share of it is shown as that subfolder (Steam, not .local).
const DOMINANT: f64 = 0.6;
const OLD_DOWNLOAD: Duration = Duration::from_secs(90 * 24 * 3600);
const MAX_OLD_ITEMS: usize = 500;

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
    pub const ALL: [Self; 8] =
        [Self::Apps, Self::Documents, Self::Pictures, Self::Videos, Self::Music, Self::Archives, Self::Caches, Self::Other];

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
        "pdf" | "doc" | "docx" | "odt" | "ods" | "odp" | "xls" | "xlsx" | "ppt" | "pptx" | "txt" | "md" | "rtf" | "epub"
        | "csv" | "tex" => Category::Documents,
        "jpg" | "jpeg" | "png" | "gif" | "webp" | "heic" | "heif" | "avif" | "jxl" | "tif" | "tiff" | "bmp" | "svg" | "raw"
        | "cr2" | "nef" | "arw" | "dng" => Category::Pictures,
        "mp4" | "mkv" | "mov" | "webm" | "avi" | "m4v" | "wmv" | "flv" | "mpg" | "mpeg" => Category::Videos,
        "mp3" | "flac" | "ogg" | "opus" | "m4a" | "wav" | "aac" | "wma" | "aiff" => Category::Music,
        "zip" | "7z" | "tar" | "gz" | "xz" | "zst" | "bz2" | "rar" | "iso" | "img" | "deb" | "rpm" => Category::Archives,
        _ => Category::Other,
    }
}

#[derive(Clone, Debug, Serialize)]
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
    scan_rooted(root, &home(), ctl)
}

/// The walk behind `scan`, with the home folder that decides caches, app data, Trash and Downloads.
fn scan_rooted(root: &Path, home: &Path, ctl: &dyn Control) -> OpResult<Summary> {
    let root_meta = fs::symlink_metadata(root).map_err(|e| OpError::Io(root.to_path_buf(), e))?;
    let device = root_meta.dev();
    let now = SystemTime::now();
    let downloads = home.join("Downloads");
    let trash = home.join(".local/share/Trash");
    let caches = home.join(".cache");

    let mut dirs: HashMap<PathBuf, u64> = HashMap::new();
    let mut per_category: HashMap<Category, u64> = HashMap::new();
    let mut seen: HashSet<(u64, u64)> = HashSet::new();
    let mut old_items = Vec::new();
    let (mut total, mut trash_bytes, mut cache_bytes, mut old_bytes, mut files) = (0u64, 0u64, 0u64, 0u64, 0u64);
    let mut todo = vec![root.to_path_buf()];
    dirs.insert(root.to_path_buf(), 0);
    while let Some(dir) = todo.pop() {
        let Ok(entries) = fs::read_dir(&dir) else { continue };
        for entry in entries.flatten() {
            if ctl.cancelled() {
                return Err(OpError::Cancelled);
            }
            let path = entry.path();
            let Ok(meta) = fs::symlink_metadata(&path) else { continue };
            if meta.dev() != device {
                continue;
            }
            if meta.is_dir() {
                dirs.insert(path.clone(), 0);
                todo.push(path);
                continue;
            }
            if meta.nlink() > 1 && !seen.insert((meta.dev(), meta.ino())) {
                continue;
            }
            let bytes = on_disk(&meta);
            total += bytes;
            files += 1;
            *per_category.entry(category(&path, home)).or_default() += bytes;
            if path.starts_with(&trash) {
                trash_bytes += bytes;
            } else if path.starts_with(&caches) {
                cache_bytes += bytes;
            } else if path.starts_with(&downloads)
                && meta.modified().is_ok_and(|m| now.duration_since(m).is_ok_and(|age| age > OLD_DOWNLOAD))
            {
                old_bytes += bytes;
                old_items.push((path.clone(), bytes));
            }
            for up in path.ancestors().skip(1) {
                if let Some(size) = dirs.get_mut(up) {
                    *size += bytes;
                }
                if up == root {
                    break;
                }
            }
            if files.is_multiple_of(4096) {
                ctl.progress(files, total, &dir);
            }
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
        scanned_at: now.duration_since(UNIX_EPOCH).map_or(0, |d| d.as_secs()),
    };
    ctl.progress(files, total, root);
    CACHE
        .lock()
        .unwrap_or_else(PoisonError::into_inner)
        .insert(root.to_path_buf(), Cache { at: now, summary: summary.clone(), dirs });
    Ok(summary)
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
        while let Some((big, big_size)) = by_parent.get(at.as_path()).and_then(|kids| kids.iter().max_by_key(|k| k.1)).copied() {
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
    let mut out: Vec<(PathBuf, u64, bool)> = children(dir, dirs).into_iter().map(|(p, s)| (p.clone(), s, true)).collect();
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
        for (p, s) in [("/h", 100), ("/h/.local", 70), ("/h/.local/share", 65), ("/h/.local/share/Steam", 60), ("/h/Videos", 30)] {
            dirs.insert(PathBuf::from(p), s);
        }
        let out = largest(&r, &dirs);
        assert_eq!(out, vec![(PathBuf::from("/h/.local/share/Steam"), 60), (PathBuf::from("/h/Videos"), 30)]);
    }

    fn scan_with_home(home: &Path, ctl: &dyn Control) -> Summary {
        scan_rooted(home, home, ctl).expect("scan")
    }
}
