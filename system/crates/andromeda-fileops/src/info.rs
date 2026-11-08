//! Get Info operations: permission changes (with undo) and checksums, both run as jobs.

use std::fs::{self, File};
use std::io::{self, Read};
use std::os::unix::fs::{MetadataExt, PermissionsExt};
use std::path::{Path, PathBuf};

use md5::Md5;
use sha2::{Digest, Sha256};

use crate::ops::{Control, OpError, OpResult, UndoStep};

const CHUNK: usize = 1 << 20;
const READ_BITS: [u32; 3] = [0o400, 0o040, 0o004];

/// The bits `path` gets: `mode` where `mask` is set, the old bits elsewhere. On a folder, open (x) follows read for
/// every class whose read bit `mask` covers, so "Read only" on a folder still lets its items be listed.
pub fn new_mode(old: u32, mode: u32, mask: u32, is_dir: bool) -> u32 {
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

/// Changes the permission bits of `paths` (and everything under them when `recursive`). Links are never changed
/// or followed: chmod would change their target.
pub fn set_permissions(paths: &[PathBuf], mode: u32, mask: u32, recursive: bool, ctl: &dyn Control) -> (Vec<UndoStep>, OpResult<()>) {
    let mut undo = Vec::new();
    let mut done = 0u64;
    let mut todo: Vec<(PathBuf, bool)> = paths.iter().rev().map(|p| (p.clone(), true)).collect();
    while let Some((path, _)) = todo.pop() {
        if ctl.cancelled() {
            return (undo, Err(OpError::Cancelled));
        }
        let meta = match fs::symlink_metadata(&path) {
            Ok(meta) => meta,
            Err(e) => return (undo, Err(OpError::Io(path, e))),
        };
        if meta.is_symlink() {
            continue;
        }
        let old = meta.mode() & 0o7777;
        let new = new_mode(old, mode, mask, meta.is_dir());
        if new != old {
            if let Err(e) = fs::set_permissions(&path, fs::Permissions::from_mode(new)) {
                return (undo, Err(OpError::Io(path, e)));
            }
            undo.push(UndoStep::Mode { path: path.clone(), mode: old });
        }
        done += 1;
        ctl.progress(done, 0, &path);
        if recursive && meta.is_dir() {
            match fs::read_dir(&path) {
                Ok(entries) => todo.extend(entries.flatten().map(|e| (e.path(), false))),
                Err(e) => return (undo, Err(OpError::Io(path, e))),
            }
        }
    }
    (undo, Ok(()))
}

/// The lowercase hex digest of `path`; algorithm "sha256" or "md5". Reads in chunks, checks cancel per chunk.
pub fn checksum(path: &Path, algorithm: &str, ctl: &dyn Control) -> OpResult<String> {
    let mut file = File::open(path).map_err(|e| OpError::Io(path.to_path_buf(), e))?;
    let mut sha = (algorithm == "sha256").then(Sha256::new);
    let mut md5 = (algorithm == "md5").then(Md5::new);
    if sha.is_none() && md5.is_none() {
        return Err(OpError::Io(path.to_path_buf(), io::Error::other(format!("unknown algorithm '{algorithm}'"))));
    }
    let mut buf = vec![0u8; CHUNK];
    let mut total = 0u64;
    loop {
        if ctl.cancelled() {
            return Err(OpError::Cancelled);
        }
        let n = file.read(&mut buf).map_err(|e| OpError::Io(path.to_path_buf(), e))?;
        if n == 0 {
            break;
        }
        if let Some(h) = sha.as_mut() {
            h.update(&buf[..n]);
        }
        if let Some(h) = md5.as_mut() {
            h.update(&buf[..n]);
        }
        total += n as u64;
        ctl.progress(0, total, path);
    }
    let digest: Vec<u8> = match (sha, md5) {
        (Some(h), _) => h.finalize().to_vec(),
        (_, Some(h)) => h.finalize().to_vec(),
        _ => Vec::new(),
    };
    Ok(digest.iter().fold(String::new(), |mut out, b| {
        use std::fmt::Write as _;
        let _ = write!(out, "{b:02x}");
        out
    }))
}

#[cfg(test)]
#[allow(clippy::expect_used)]
mod tests {
    use super::*;
    use crate::ops::{self, Resolution};

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

    fn mode_of(path: &Path) -> u32 {
        fs::symlink_metadata(path).expect("stat").mode() & 0o777
    }

    #[test]
    fn read_only_for_everyone_on_a_folder_keeps_it_openable() {
        assert_eq!(new_mode(0o755, 0o004, 0o006, true), 0o755);
        assert_eq!(new_mode(0o755, 0o000, 0o006, true), 0o750);
        assert_eq!(new_mode(0o644, 0o000, 0o006, false), 0o640);
        assert_eq!(new_mode(0o640, 0o006, 0o006, false), 0o646);
    }

    #[test]
    fn set_permissions_reaches_enclosed_items_and_undo_puts_them_back() {
        let root = tempfile::tempdir().expect("tmp");
        let dir = root.path().join("d");
        fs::create_dir(&dir).expect("mkdir");
        fs::write(dir.join("f"), "x").expect("write");
        fs::set_permissions(&dir, fs::Permissions::from_mode(0o755)).expect("chmod");
        fs::set_permissions(dir.join("f"), fs::Permissions::from_mode(0o644)).expect("chmod");
        std::os::unix::fs::symlink("/etc/hostname", dir.join("link")).expect("link");
        let (undo, result) = set_permissions(&[dir.clone()], 0o000, 0o007, true, &Quietly);
        result.expect("chmod");
        assert_eq!((mode_of(&dir), mode_of(&dir.join("f"))), (0o750, 0o640));
        let (_, result, _) = ops::undo(&undo, &Quietly);
        result.expect("undo");
        assert_eq!((mode_of(&dir), mode_of(&dir.join("f"))), (0o755, 0o644));
    }

    #[test]
    fn checksums_match_known_digests() {
        let root = tempfile::tempdir().expect("tmp");
        let file = root.path().join("abc.txt");
        fs::write(&file, "abc").expect("write");
        assert_eq!(
            checksum(&file, "sha256", &Quietly).expect("sha"),
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
        );
        assert_eq!(checksum(&file, "md5", &Quietly).expect("md5"), "900150983cd24fb0d6963f7d28e17f72");
    }
}
