//! The freedesktop.org Trash specification 1.0: home trash, per-device `$topdir/.Trash/$uid` and `.Trash-$uid`.

use std::ffi::OsString;
use std::fs::{self, OpenOptions};
use std::io::{self, Write};
use std::os::unix::ffi::{OsStrExt, OsStringExt};
use std::os::unix::fs::{DirBuilderExt, MetadataExt, OpenOptionsExt};
use std::path::{Component, Path, PathBuf};
use std::sync::OnceLock;

use percent_encoding::{AsciiSet, CONTROLS, percent_decode_str, percent_encode};
use rustix::fs::{CWD, RenameFlags, renameat_with};

// RFC 2396 path escaping, as the spec asks: keep '/' and unreserved characters.
const PATH_SET: &AsciiSet = &CONTROLS
    .add(b' ')
    .add(b'"')
    .add(b'#')
    .add(b'%')
    .add(b'<')
    .add(b'>')
    .add(b'?')
    .add(b'[')
    .add(b'\\')
    .add(b']')
    .add(b'^')
    .add(b'`')
    .add(b'{')
    .add(b'|')
    .add(b'}');

/// One trashed item: where it went and where it came from.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Trashed {
    pub trash_dir: PathBuf,
    pub name: String,
    pub original: PathBuf,
}

impl Trashed {
    pub fn file(&self) -> PathBuf {
        self.trash_dir.join("files").join(&self.name)
    }

    pub fn info(&self) -> PathBuf {
        self.trash_dir.join("info").join(format!("{}.trashinfo", self.name))
    }
}

static HOME_TRASH: OnceLock<Option<PathBuf>> = OnceLock::new();

fn home_trash() -> io::Result<PathBuf> {
    HOME_TRASH
        .get_or_init(|| {
            let data = std::env::var_os("XDG_DATA_HOME")
                .map(PathBuf::from)
                .filter(|p| p.is_absolute())
                .or_else(|| std::env::var_os("HOME").map(|home| PathBuf::from(home).join(".local/share")))?;
            Some(data.join("Trash"))
        })
        .clone()
        .ok_or_else(|| io::Error::other("no HOME"))
}

#[cfg(test)]
pub fn use_home_trash(dir: PathBuf) {
    let _ = HOME_TRASH.set(Some(dir));
}

fn uid() -> u32 {
    rustix::process::getuid().as_raw()
}

/// The highest folder of `path`'s mount: walk up while the device stays the same. `path` must hold no links.
fn top_dir(path: &Path, dev: u64) -> PathBuf {
    let mut top = path.to_path_buf();
    while let Some(parent) = top.parent() {
        if fs::metadata(parent).map(|m| m.dev()).ok() != Some(dev) {
            break;
        }
        top = parent.to_path_buf();
    }
    top
}

fn ensure_trash(dir: &Path) -> io::Result<()> {
    for sub in ["files", "info"] {
        fs::DirBuilder::new().recursive(true).mode(0o700).create(dir.join(sub))?;
    }
    Ok(())
}

/// A per-device trash someone else could have made (GIO's checks): the folder, files and info must be real folders
/// owned by this user. No mode check: FAT, exFAT and NTFS drives show every folder as 0777.
fn check_own_trash(dir: &Path) -> io::Result<()> {
    for path in [dir.to_path_buf(), dir.join("files"), dir.join("info")] {
        let meta = fs::symlink_metadata(&path)?;
        if !meta.is_dir() || meta.uid() != uid() {
            return Err(io::Error::other(format!("{} is not a trash folder of this user", path.display())));
        }
    }
    Ok(())
}

/// The trash for `path` and the name its info file stores: absolute for the home trash, relative to the top
/// folder for a per-device trash.
fn trash_for(path: &Path) -> io::Result<(PathBuf, PathBuf)> {
    let dev = fs::symlink_metadata(path)?.dev();
    let home = home_trash()?;
    ensure_trash(&home)?;
    if dev == fs::metadata(&home)?.dev() {
        return Ok((home, path.to_path_buf()));
    }
    let name = path.file_name().ok_or_else(|| io::Error::from(io::ErrorKind::InvalidInput))?;
    let real = fs::canonicalize(path.parent().unwrap_or(Path::new("/")))?.join(name);
    let top = top_dir(real.parent().unwrap_or(Path::new("/")), dev);
    let relative = real.strip_prefix(&top).map_or_else(|_| real.clone(), Path::to_path_buf);
    // $topdir/.Trash must be a real folder with the sticky bit, else it is not used.
    let shared = top.join(".Trash");
    if let Ok(meta) = fs::symlink_metadata(&shared)
        && meta.is_dir()
        && meta.mode() & 0o1000 != 0
    {
        let dir = shared.join(uid().to_string());
        if ensure_trash(&dir).and_then(|()| check_own_trash(&dir)).is_ok() {
            return Ok((dir, relative));
        }
    }
    let own = top.join(format!(".Trash-{}", uid()));
    if fs::symlink_metadata(&own).is_ok_and(|m| m.file_type().is_symlink()) {
        return Err(io::Error::other("trash folder is a link"));
    }
    ensure_trash(&own)?;
    check_own_trash(&own)?;
    Ok((own, relative))
}

/// `name` cut at a character boundary so `<name>.<u32>.trashinfo` fits `NAME_MAX` (255 bytes); the info file keeps
/// the real name.
fn shortened(name: &str) -> &str {
    const MAX: usize = 255 - ".trashinfo".len() - ".4294967295".len();
    let mut end = name.len().min(MAX);
    while !name.is_char_boundary(end) {
        end -= 1;
    }
    &name[..end]
}

/// Moves `path` to its trash. The info file is made first with `O_EXCL`, which reserves the name.
pub fn trash(path: &Path) -> io::Result<Trashed> {
    let path = absolute(path)?;
    let (trash_dir, stored) = trash_for(&path)?;
    let base = path.file_name().ok_or_else(|| io::Error::from(io::ErrorKind::InvalidInput))?.to_string_lossy();
    let base = shortened(&base);
    let date = chrono::Local::now().format("%Y-%m-%dT%H:%M:%S");
    let encoded = percent_encode(stored.as_os_str().as_bytes(), PATH_SET).to_string();
    for n in 1u32.. {
        let name = if n == 1 { base.to_string() } else { format!("{base}.{n}") };
        let entry = Trashed { trash_dir: trash_dir.clone(), name, original: path.clone() };
        let mut info = match OpenOptions::new().write(true).create_new(true).mode(0o600).open(entry.info()) {
            Ok(file) => file,
            Err(e) if e.kind() == io::ErrorKind::AlreadyExists => continue,
            Err(e) => return Err(e),
        };
        info.write_all(format!("[Trash Info]\nPath={encoded}\nDeletionDate={date}\n").as_bytes())?;
        drop(info);
        return match renameat_with(CWD, &path, CWD, entry.file(), RenameFlags::NOREPLACE) {
            Ok(()) => Ok(entry),
            Err(e) => {
                let _ = fs::remove_file(entry.info());
                Err(e.into())
            }
        };
    }
    unreachable!("u32 names run out")
}

/// Puts a trashed item back. A taken original path gets a "(copy)" name; a missing parent folder is made.
pub fn restore(entry: &Trashed) -> io::Result<PathBuf> {
    let parent = entry.original.parent().unwrap_or(Path::new("/"));
    fs::create_dir_all(parent)?;
    let target = if fs::symlink_metadata(&entry.original).is_ok() {
        crate::ops::free_name(&entry.original)
    } else {
        entry.original.clone()
    };
    renameat_with(CWD, entry.file(), CWD, &target, RenameFlags::NOREPLACE)?;
    fs::remove_file(entry.info())?;
    Ok(target)
}

/// Reads the original path from an info file, for items trashed by another program.
pub fn read_info(trash_dir: &Path, name: &str) -> io::Result<Trashed> {
    let text = fs::read_to_string(trash_dir.join("info").join(format!("{name}.trashinfo")))?;
    // Only the [Trash Info] group counts (spec 1.0); a Path= in another group is not the original location.
    let stored = text
        .lines()
        .skip_while(|line| line.trim() != "[Trash Info]")
        .skip(1)
        .take_while(|line| !line.trim_start().starts_with('['))
        .find_map(|line| line.strip_prefix("Path="))
        .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidData, "no Path="))?;
    let stored = PathBuf::from(OsString::from_vec(percent_decode_str(stored).collect()));
    let original = if stored.is_absolute() {
        stored
    } else {
        // Per-device trash: $topdir/.Trash/$uid or $topdir/.Trash-$uid.
        let top = if trash_dir.parent().and_then(Path::file_name).is_some_and(|n| n == ".Trash") {
            trash_dir.parent().and_then(Path::parent)
        } else {
            trash_dir.parent()
        };
        top.unwrap_or(Path::new("/")).join(stored)
    };
    if original.components().any(|c| c == Component::ParentDir) {
        return Err(io::Error::new(io::ErrorKind::InvalidData, "'..' in Path="));
    }
    Ok(Trashed { trash_dir: trash_dir.to_path_buf(), name: name.to_owned(), original })
}

fn absolute(path: &Path) -> io::Result<PathBuf> {
    if path.is_absolute() { Ok(path.to_path_buf()) } else { Ok(std::env::current_dir()?.join(path)) }
}

#[cfg(test)]
#[allow(clippy::expect_used)]
mod tests {
    use super::*;

    #[test]
    fn path_comes_only_from_the_trash_info_group() {
        let dir = tempfile::tempdir().expect("tmp");
        fs::create_dir_all(dir.path().join("info")).expect("mkdir");
        fs::write(
            dir.path().join("info/a.trashinfo"),
            "[Other]\nPath=/etc/passwd\n[Trash Info]\nPath=/home/u/a%20b\nDeletionDate=2026-11-05T07:00:00\n",
        )
        .expect("write");
        assert_eq!(read_info(dir.path(), "a").expect("info").original, PathBuf::from("/home/u/a b"));
        fs::write(dir.path().join("info/b.trashinfo"), "[Other]\nPath=/etc/passwd\n").expect("write");
        assert!(read_info(dir.path(), "b").is_err());
    }

    #[test]
    fn a_link_in_the_parents_does_not_move_the_device_trash() {
        let home = tempfile::tempdir().expect("tmp");
        use_home_trash(home.path().join("Trash"));
        let other = tempfile::tempdir_in("/dev/shm").expect("shm");
        if fs::metadata(other.path()).expect("stat").dev() == fs::metadata(home.path()).expect("stat").dev() {
            return;
        }
        fs::create_dir(other.path().join("sub")).expect("mkdir");
        fs::write(other.path().join("sub/f"), "x").expect("write");
        std::os::unix::fs::symlink(other.path().join("sub"), home.path().join("lnk")).expect("link");
        let made = !Path::new("/dev/shm").join(format!(".Trash-{}", uid())).exists();
        let (dir, relative) = trash_for(&home.path().join("lnk/f")).expect("trash");
        if made {
            let _ = fs::remove_dir_all(&dir);
        }
        assert_eq!(dir.parent(), Some(Path::new("/dev/shm")));
        assert_eq!(relative, other.path().strip_prefix("/dev/shm").expect("in shm").join("sub/f"));
    }
}
