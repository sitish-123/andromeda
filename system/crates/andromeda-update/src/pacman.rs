//! Repository updates without root and without touching the real database: sync a copy of it, as `checkupdates`
//! (pacman-contrib) does, so a check can never cause a partial upgrade.

use std::collections::HashMap;
use std::path::Path;

use anyhow::{Context, Result, bail};
use tokio::process::Command;

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Update {
    pub name: String,
    pub old: String,
    pub new: String,
    pub size: u64,
    /// From the Andromeda repository.
    pub ours: bool,
}

/// `pacman -Qu` lines: `name old -> new`, optionally followed by ` [ignored]` (`IgnorePkg`: held, not upgraded).
/// Returns (upgrades, held).
pub fn parse_upgrades(text: &str) -> (Vec<Update>, Vec<Update>) {
    let (mut updates, mut held) = (Vec::new(), Vec::new());
    for line in text.lines() {
        let mut parts = line.split_whitespace();
        let (Some(name), Some(old), Some("->"), Some(new)) = (parts.next(), parts.next(), parts.next(), parts.next())
        else {
            continue;
        };
        let update = Update { name: name.into(), old: old.into(), new: new.into(), size: 0, ours: false };
        match parts.next() {
            None => updates.push(update),
            Some("[ignored]") => held.push(update),
            Some(_) => {}
        }
    }
    (updates, held)
}

/// `pacman -Sup --print-format '%n %s %r'` lines: name, download size in bytes and repository.
pub fn parse_sizes(text: &str) -> HashMap<String, (u64, String)> {
    text.lines()
        .filter_map(|line| {
            let mut parts = line.split_whitespace();
            let (name, size) = (parts.next()?, parts.next()?.parse().ok()?);
            Some((name.to_owned(), (size, parts.next().unwrap_or_default().to_owned())))
        })
        .collect()
}

/// Lists upgrades with `checkupdates` (pacman-contrib), which syncs a private copy of the databases in `db`.
pub async fn check(db: &Path) -> Result<(Vec<Update>, Vec<Update>)> {
    std::fs::create_dir_all(db)?;
    let out = Command::new("checkupdates")
        .arg("--nocolor")
        .env("CHECKUPDATES_DB", db)
        .env("LC_ALL", "C")
        .output()
        .await
        .context("checkupdates (pacman-contrib)")?;
    // 0: updates, 2: none, anything else: failure.
    match out.status.code() {
        Some(2) => return Ok((Vec::new(), Vec::new())),
        Some(0) => {}
        _ => bail!("checkupdates: {}", String::from_utf8_lossy(&out.stderr).trim()),
    }
    let (mut updates, held) = parse_upgrades(&String::from_utf8_lossy(&out.stdout));
    let sizes = Command::new("pacman")
        .args(["-Sup", "--print-format", "%n %s %r", "--dbpath"])
        .arg(db)
        .env("LC_ALL", "C")
        .output()
        .await
        .context("pacman -Sup")?;
    if sizes.status.success() {
        let sizes = parse_sizes(&String::from_utf8_lossy(&sizes.stdout));
        for update in &mut updates {
            if let Some((size, repo)) = sizes.get(&update.name) {
                update.size = *size;
                update.ours = repo == andromeda_update::repo::PACMAN_REPO;
            }
        }
    }
    Ok((updates, held))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn ignored_packages_are_held_and_noise_is_skipped() {
        let text = "linux 6.9.1-1 -> 6.9.2-1\nmesa 1:24.1-1 -> 1:24.2-1 [ignored]\nwarning: something\n";
        let (updates, held) = parse_upgrades(text);
        assert_eq!(updates.len(), 1);
        assert_eq!((updates[0].name.as_str(), updates[0].new.as_str()), ("linux", "6.9.2-1"));
        assert_eq!((held.len(), held[0].name.as_str()), (1, "mesa"));
    }

    #[test]
    fn sizes_by_name() {
        let sizes = parse_sizes("linux 140123456 core\nbroken line\nmesa 0 extra\n");
        assert_eq!(sizes.get("linux"), Some(&(140_123_456, "core".to_owned())));
        assert_eq!(sizes.get("mesa").map(|s| s.0), Some(0));
        assert_eq!(sizes.len(), 2);
    }
}
