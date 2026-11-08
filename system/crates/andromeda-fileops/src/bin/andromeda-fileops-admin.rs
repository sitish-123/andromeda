//! Root helper for Get Info (FB-INFO): changes the mode of files the user does not own, after polkit
//! (action org.andromeda.files.admin) let pkexec start it. The arguments come from an unprivileged caller, so every
//! path is checked: absolute, no `.` or `..` parts, not under /proc, /sys or /dev; links are never changed or followed.
//!
//! andromeda-fileops-admin chmod <mode octal> <mask octal> <0|1 recursive> <path>...
//! Prints "done <count>" and exits 0, or prints the error and exits 1.

use rustix::fs::{chmodat, fstat, openat, AtFlags, Dir, FileType, Mode, OFlags, CWD};
use std::os::fd::{AsRawFd, OwnedFd};
use std::os::unix::ffi::OsStrExt;
use std::path::{Component, Path, PathBuf};
use std::process::ExitCode;

const READ_BITS: [u32; 3] = [0o400, 0o040, 0o004];
const REFUSED_ROOTS: [&str; 3] = ["/proc", "/sys", "/dev"];

struct Change {
    mode: u32,
    mask: u32,
}

/// The same rule as the daemon (info.rs new_mode): a folder's search bit follows its read bit.
fn new_mode(old: u32, mode: u32, mask: u32, is_dir: bool) -> u32 {
    let mut out = (old & !mask) | (mode & mask);
    if is_dir {
        for read in READ_BITS {
            if mask & read != 0 {
                let exec = read >> 2;
                out = if out & read != 0 { out | exec } else { out & !exec };
            }
        }
    }
    out & 0o7777
}

fn checked(path: &str) -> Result<PathBuf, String> {
    let p = Path::new(path);
    if !p.is_absolute() {
        return Err(format!("{path}: not an absolute path"));
    }
    if p.components().any(|c| matches!(c, Component::CurDir | Component::ParentDir)) {
        return Err(format!("{path}: . and .. are not allowed"));
    }
    if REFUSED_ROOTS.iter().any(|root| p.starts_with(root)) || p == Path::new("/") {
        return Err(format!("{path}: refused"));
    }
    Ok(p.to_path_buf())
}

/// Opens each component from `/` with O_NOFOLLOW, so no link anywhere on the path is followed: a user who can write a
/// parent folder cannot swap it for a link to /etc between our check and the change.
fn open_parent(path: &Path) -> Result<(OwnedFd, std::ffi::OsString), String> {
    let name = path.file_name().ok_or_else(|| format!("{}: no file name", path.display()))?.to_owned();
    let mut dir = openat(CWD, "/", OFlags::DIRECTORY | OFlags::RDONLY | OFlags::CLOEXEC, Mode::empty())
        .map_err(|e| format!("/: {e}"))?;
    for part in path.parent().unwrap_or(Path::new("/")).components() {
        let Component::Normal(part) = part else { continue };
        dir = openat(&dir, part, OFlags::DIRECTORY | OFlags::NOFOLLOW | OFlags::RDONLY | OFlags::CLOEXEC, Mode::empty())
            .map_err(|e| format!("{}: {e}", path.display()))?;
    }
    Ok((dir, name))
}

fn apply(path: &Path, change: &Change, recursive: bool, count: &mut u64) -> Result<(), String> {
    let (dir, name) = open_parent(path)?;
    apply_at(&dir, &name, path, change, recursive, count)
}

/// Every step is relative to an open folder and never follows a link (fstatat, fchmodat with AT_SYMLINK_NOFOLLOW,
/// openat with O_NOFOLLOW), and a folder is entered only if it is still the one that was stat'ed.
fn apply_at(dir: &OwnedFd, name: &std::ffi::OsStr, shown: &Path, change: &Change, recursive: bool, count: &mut u64) -> Result<(), String> {
    let err = |e: rustix::io::Errno| format!("{}: {e}", shown.display());
    // O_PATH + NOFOLLOW pins the inode; a symlink swapped in later cannot redirect the chmod.
    let pinned = openat(dir, name, OFlags::PATH | OFlags::NOFOLLOW | OFlags::CLOEXEC, Mode::empty()).map_err(err)?;
    let st = fstat(&pinned).map_err(err)?;
    let kind = FileType::from_raw_mode(st.st_mode);
    if kind == FileType::Symlink {
        return Ok(());
    }
    let is_dir = kind == FileType::Directory;
    let old = st.st_mode & 0o7777;
    let new = new_mode(old, change.mode, change.mask, is_dir);
    if new != old {
        // fchmod refuses O_PATH fds; /proc/self/fd/N resolves to the pinned inode (glibc's fchmodat fallback does this).
        let proc = format!("/proc/self/fd/{}", pinned.as_raw_fd());
        chmodat(CWD, proc.as_str(), Mode::from_raw_mode(new), AtFlags::empty()).map_err(err)?;
    }
    *count += 1;
    if recursive && is_dir {
        let child = openat(dir, name, OFlags::DIRECTORY | OFlags::NOFOLLOW | OFlags::RDONLY | OFlags::CLOEXEC, Mode::empty())
            .map_err(err)?;
        let now = fstat(&child).map_err(err)?;
        if now.st_ino != st.st_ino || now.st_dev != st.st_dev {
            return Err(format!("{}: changed while it was being changed", shown.display()));
        }
        let mut entries = Dir::read_from(&child).map_err(err)?;
        while let Some(entry) = entries.next() {
            let entry = entry.map_err(err)?;
            let bytes = entry.file_name().to_bytes();
            if bytes == b"." || bytes == b".." {
                continue;
            }
            let part = std::ffi::OsStr::from_bytes(bytes);
            apply_at(&child, part, &shown.join(part), change, true, count)?;
        }
    }
    Ok(())
}

fn run(args: &[String]) -> Result<u64, String> {
    let [verb, a, b, recursive, paths @ ..] = args else {
        return Err("usage: chmod <mode> <mask> <0|1> <path>...".to_owned());
    };
    if paths.is_empty() {
        return Err("no paths".to_owned());
    }
    if verb != "chmod" {
        return Err(format!("unknown verb {verb}"));
    }
    let change = Change {
        mode: u32::from_str_radix(a, 8).map_err(|_| "bad mode".to_owned())? & 0o7777,
        mask: u32::from_str_radix(b, 8).map_err(|_| "bad mask".to_owned())? & 0o7777,
    };
    let recursive = match recursive.as_str() {
        "0" => false,
        "1" => true,
        _ => return Err("recursive is 0 or 1".to_owned()),
    };
    let checked: Vec<PathBuf> = paths.iter().map(|p| checked(p)).collect::<Result<_, _>>()?;
    let mut count = 0;
    for path in &checked {
        apply(path, &change, recursive, &mut count)?;
    }
    Ok(count)
}

fn main() -> ExitCode {
    if !rustix::process::geteuid().is_root() {
        eprintln!("andromeda-fileops-admin runs only through pkexec");
        return ExitCode::FAILURE;
    }
    let args: Vec<String> = std::env::args().skip(1).collect();
    match run(&args) {
        Ok(count) => {
            println!("done {count}");
            ExitCode::SUCCESS
        }
        Err(error) => {
            eprintln!("{error}");
            ExitCode::FAILURE
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs;
    use std::os::unix::fs::{MetadataExt, PermissionsExt};

    #[test]
    fn checked_refuses_relative_dotdot_and_kernel_trees() {
        assert!(checked("rel/x").is_err());
        assert!(checked("/home/a/../etc/shadow").is_err());
        assert!(checked("/proc/1/mem").is_err());
        assert!(checked("/").is_err());
        assert!(checked("/home/a/file").is_ok());
    }

    #[test]
    fn chmod_skips_links_and_recurses() {
        let root = tempfile::tempdir().expect("tmp");
        let dir = root.path().join("d");
        fs::create_dir(&dir).expect("mkdir");
        fs::write(dir.join("f"), "x").expect("write");
        fs::set_permissions(dir.join("f"), fs::Permissions::from_mode(0o600)).expect("chmod");
        let target = root.path().join("outside");
        fs::write(&target, "x").expect("write");
        fs::set_permissions(&target, fs::Permissions::from_mode(0o600)).expect("chmod");
        std::os::unix::fs::symlink(&target, dir.join("link")).expect("link");
        let mut count = 0;
        apply(&dir, &Change { mode: 0o044, mask: 0o044 }, true, &mut count).expect("apply");
        assert_eq!(fs::metadata(dir.join("f")).expect("meta").mode() & 0o777, 0o644);
        assert_eq!(fs::metadata(&target).expect("meta").mode() & 0o777, 0o600, "the link target must not change");
    }
}
