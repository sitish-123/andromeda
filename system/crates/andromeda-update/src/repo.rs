//! The Andromeda package repository that install.sh adds: its updates form their own group, and the helper can update
//! them alone.

use std::collections::HashSet;
use std::path::Path;

/// The pacman repository name in /etc/pacman.conf.
pub const PACMAN_REPO: &str = "andromeda";
/// The apt source file install.sh writes.
pub const APT_SOURCE: &str = "/etc/apt/sources.list.d/andromeda.list";

/// The lists file name apt gives the flat repository on a `deb [options] URL ./` line: scheme dropped, `/` as `_`.
pub fn apt_packages_file(source_line: &str) -> Option<String> {
    let url = source_line.split_whitespace().find(|w| w.contains("://"))?;
    let path = url.split_once("://")?.1.trim_end_matches('/');
    Some(format!("{}_._Packages", path.replace('/', "_")))
}

/// `Package:` names in an apt Packages index.
pub fn apt_package_names(index: &str) -> HashSet<String> {
    index.lines().filter_map(|l| l.strip_prefix("Package: ")).map(|n| n.trim().to_owned()).collect()
}

/// Package names the Andromeda apt source offers, read from the lists directory; empty without the source.
pub fn apt_names(lists: &Path) -> HashSet<String> {
    let Ok(source) = std::fs::read_to_string(APT_SOURCE) else { return HashSet::new() };
    let Some(file) = source.lines().filter(|l| l.starts_with("deb ")).find_map(apt_packages_file) else {
        return HashSet::new();
    };
    std::fs::read_to_string(lists.join(file)).map(|t| apt_package_names(&t)).unwrap_or_default()
}
