//! The freedesktop thumbnail cache layout (Thumbnail Managing Standard 0.9).

use md5::{Digest, Md5};
use std::path::{Path, PathBuf};

/// Our directory under `fail/`: each program keeps its own failures, since another one may succeed.
pub const FAIL_DIR: &str = "andromeda-thumbnailer-1";

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Flavor {
    Normal,
    Large,
    XLarge,
    XxLarge,
}

impl Flavor {
    #[must_use]
    pub fn parse(name: &str) -> Option<Self> {
        Some(match name {
            "normal" => Self::Normal,
            "large" => Self::Large,
            "x-large" => Self::XLarge,
            "xx-large" => Self::XxLarge,
            _ => return None,
        })
    }

    #[must_use]
    pub const fn dir(self) -> &'static str {
        match self {
            Self::Normal => "normal",
            Self::Large => "large",
            Self::XLarge => "x-large",
            Self::XxLarge => "xx-large",
        }
    }

    #[must_use]
    pub const fn size(self) -> u32 {
        match self {
            Self::Normal => 128,
            Self::Large => 256,
            Self::XLarge => 512,
            Self::XxLarge => 1024,
        }
    }

    pub const ALL: [Self; 4] = [Self::Normal, Self::Large, Self::XLarge, Self::XxLarge];
}

fn file_name(uri: &str) -> String {
    let digest = Md5::digest(uri.as_bytes());
    use std::fmt::Write;
    let mut name = String::with_capacity(36);
    for b in digest {
        let _ = write!(name, "{b:02x}");
    }
    name.push_str(".png");
    name
}

#[must_use]
pub fn thumbnail_path(root: &Path, flavor: Flavor, uri: &str) -> PathBuf {
    root.join(flavor.dir()).join(file_name(uri))
}

#[must_use]
pub fn fail_path(root: &Path, uri: &str) -> PathBuf {
    root.join("fail").join(FAIL_DIR).join(file_name(uri))
}

/// `$XDG_CACHE_HOME/thumbnails`, else `~/.cache/thumbnails`.
#[must_use]
pub fn default_root() -> Option<PathBuf> {
    std::env::var_os("XDG_CACHE_HOME")
        .filter(|v| !v.is_empty())
        .map(PathBuf::from)
        .or_else(|| std::env::var_os("HOME").map(|h| PathBuf::from(h).join(".cache")))
        .map(|c| c.join("thumbnails"))
}
