//! Tags in the freedesktop xattr `user.xdg.tags` (a comma list, as KDE Baloo and GNOME Files read it).

use std::io;
use std::path::{Path, PathBuf};

use rustix::fs::{XattrFlags, getxattr, removexattr, setxattr};
use rustix::io::Errno;

use crate::ops::{Control, OpError, OpResult, UndoStep};

pub const XATTR: &str = "user.xdg.tags";
const MAX_VALUE: usize = 64 * 1024;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Mode {
    /// The item has exactly these tags.
    Set,
    Add,
    Remove,
    /// `tags` = [old, new]: old becomes new in place (a tag renamed in the tag manager).
    Rename,
}

impl Mode {
    pub fn parse(s: &str) -> Option<Self> {
        match s {
            "set" => Some(Self::Set),
            "add" => Some(Self::Add),
            "remove" => Some(Self::Remove),
            "rename" => Some(Self::Rename),
            _ => None,
        }
    }
}

/// A tag name: not empty, no comma (the list separator), no control characters.
pub fn valid_tag(tag: &str) -> bool {
    !tag.trim().is_empty() && !tag.contains(',') && !tag.chars().any(char::is_control)
}

/// The raw xattr value; `None` when the item has none.
pub fn read_raw(path: &Path) -> io::Result<Option<Vec<u8>>> {
    let mut buf = vec![0u8; MAX_VALUE];
    match getxattr(path, XATTR, &mut buf[..]) {
        Ok(n) => {
            buf.truncate(n);
            Ok(Some(buf))
        }
        Err(Errno::NODATA) => Ok(None),
        Err(e) => Err(e.into()),
    }
}

pub fn parse(raw: &[u8]) -> Vec<String> {
    let mut out: Vec<String> = Vec::new();
    for tag in String::from_utf8_lossy(raw).split(',') {
        let tag = tag.trim();
        if !tag.is_empty() && !out.iter().any(|t| t == tag) {
            out.push(tag.to_owned());
        }
    }
    out
}

/// Writes `raw` back (`None` removes the xattr).
pub fn write_raw(path: &Path, raw: Option<&[u8]>) -> io::Result<()> {
    match raw {
        Some(value) => setxattr(path, XATTR, value, XattrFlags::empty()).map_err(Into::into),
        None => match removexattr(path, XATTR) {
            Ok(()) | Err(Errno::NODATA) => Ok(()),
            Err(e) => Err(e.into()),
        },
    }
}

/// Changes the tags of `paths`; undo puts back each item's old value.
pub fn set_tags(paths: &[PathBuf], tags: &[String], mode: Mode, ctl: &dyn Control) -> (Vec<UndoStep>, OpResult<()>) {
    let mut undo = Vec::new();
    for (i, path) in paths.iter().enumerate() {
        if ctl.cancelled() {
            return (undo, Err(OpError::Cancelled));
        }
        let old = match read_raw(path) {
            Ok(old) => old,
            Err(e) => return (undo, Err(OpError::Io(path.clone(), e))),
        };
        let mut now = old.as_deref().map(parse).unwrap_or_default();
        match mode {
            Mode::Set => now = tags.to_vec(),
            Mode::Add => {
                for tag in tags {
                    if !now.contains(tag) {
                        now.push(tag.clone());
                    }
                }
            }
            Mode::Remove => now.retain(|t| !tags.contains(t)),
            Mode::Rename => {
                if let [old, new] = tags {
                    if now.contains(old) {
                        now.retain(|t| t != new);
                        for t in &mut now {
                            if t == old {
                                t.clone_from(new);
                            }
                        }
                    }
                }
            }
        }
        let new = (!now.is_empty()).then(|| now.join(",").into_bytes());
        if new != old {
            if let Err(e) = write_raw(path, new.as_deref()) {
                return (undo, Err(OpError::Io(path.clone(), e)));
            }
            undo.push(UndoStep::Tags { path: path.clone(), raw: old });
        }
        ctl.progress(i as u64 + 1, 0, path);
    }
    (undo, Ok(()))
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

    fn tags_of(path: &Path) -> Vec<String> {
        read_raw(path).expect("read").as_deref().map(parse).unwrap_or_default()
    }

    #[test]
    fn tags_round_trip_and_undo_restores_the_old_value() {
        // tmpfs takes user xattrs since Linux 6.6; a home folder test dir does too.
        let root = tempfile::tempdir_in(
            std::env::var_os("CARGO_TARGET_TMPDIR").map_or_else(std::env::temp_dir, PathBuf::from),
        )
        .expect("tmp");
        let file = root.path().join("a.txt");
        std::fs::write(&file, "x").expect("write");
        if write_raw(&file, Some(b"Work")).is_err() {
            eprintln!("this file system has no user xattrs; skipping");
            return;
        }
        let (_, result) = set_tags(std::slice::from_ref(&file), &["Urgent".into(), "Work".into()], Mode::Add, &Quietly);
        result.expect("add");
        assert_eq!(tags_of(&file), vec!["Work", "Urgent"], "add keeps the old tag and adds the new one once");
        let (undo, result) = set_tags(std::slice::from_ref(&file), &["Work".into()], Mode::Remove, &Quietly);
        result.expect("remove");
        assert_eq!(tags_of(&file), vec!["Urgent"]);
        let (_, result, _) = ops::undo(&undo, &Quietly);
        result.expect("undo");
        assert_eq!(tags_of(&file), vec!["Work", "Urgent"], "undo puts back the exact old list");
        let (undo, result) = set_tags(std::slice::from_ref(&file), &[], Mode::Set, &Quietly);
        result.expect("clear");
        assert_eq!(read_raw(&file).expect("read"), None, "no tags removes the xattr");
        ops::undo(&undo, &Quietly).1.expect("undo clear");
        assert_eq!(tags_of(&file), vec!["Work", "Urgent"]);
    }

    #[test]
    fn a_list_from_other_apps_is_read_trimmed_and_without_repeats() {
        assert_eq!(parse(b" Work, Urgent ,,Work"), vec!["Work", "Urgent"]);
        assert!(!valid_tag("a,b") && !valid_tag("  ") && valid_tag("Family"));
    }
}
