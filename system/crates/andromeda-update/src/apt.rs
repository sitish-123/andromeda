//! Repository updates without root and without touching the system lists: `apt-get update` into a private lists
//! directory, then a simulated `dist-upgrade` against it, the apt equal of `checkupdates`. apt applies phased updates
//! and holds as it would for a real upgrade.

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
    /// From the Andromeda apt source.
    pub ours: bool,
}

/// `apt-get -s` lines: `Inst name [old] (new origin [arch])`; a new package has no `[old]`.
pub fn parse_upgrades(text: &str) -> Vec<Update> {
    text.lines()
        .filter_map(|line| {
            let rest = line.strip_prefix("Inst ")?;
            let (name, rest) = rest.split_once(' ')?;
            let (old, rest) = match rest.strip_prefix('[') {
                Some(r) => {
                    let (old, r) = r.split_once("] ")?;
                    (old, r)
                }
                None => ("", rest),
            };
            let new = rest.strip_prefix('(')?.split_whitespace().next()?;
            Some(Update { name: name.into(), old: old.into(), new: new.into(), size: 0, ours: false })
        })
        .collect()
}

/// Package names under the simulation's "kept back" and "deferred due to phasing" headers: apt leaves these out of
/// the upgrade, so they show as their own group, not as installable rows.
pub fn parse_held(text: &str) -> Vec<String> {
    let mut held = Vec::new();
    let mut inside = false;
    for line in text.lines() {
        if line.starts_with("  ") {
            if inside {
                held.extend(line.split_whitespace().map(str::to_owned));
            }
        } else {
            inside = line.contains("kept back:") || line.contains("deferred due to phasing:");
        }
    }
    held
}

/// `apt-cache policy` blocks: a column-0 `name:` line, then `  Installed: v` and `  Candidate: v`.
pub fn parse_policy(text: &str) -> HashMap<String, (String, String)> {
    let mut out = HashMap::new();
    let mut name = None;
    for line in text.lines() {
        if let Some(n) = line.strip_suffix(':').filter(|_| !line.starts_with(' ')) {
            name = Some(n.to_owned());
        } else if let Some(n) = &name {
            let entry = out.entry(n.clone()).or_insert_with(|| (String::new(), String::new()));
            if let Some(v) = line.trim().strip_prefix("Installed: ") {
                entry.0 = v.into();
            } else if let Some(v) = line.trim().strip_prefix("Candidate: ") {
                entry.1 = v.into();
            }
        }
    }
    out
}

/// `apt-get --print-uris` lines: `'uri' name_version_arch.deb size hash`.
pub fn parse_sizes(text: &str) -> HashMap<String, u64> {
    text.lines()
        .filter_map(|line| {
            let mut parts = line.split_whitespace().skip(1);
            let (file, size) = (parts.next()?, parts.next()?);
            Some((file.split_once('_')?.0.to_owned(), size.parse().ok()?))
        })
        .collect()
}

async fn apt(tool: &str, db: &Path, args: &[&str]) -> Result<std::process::Output> {
    let opt = |key: &str, dir: &str| format!("{key}={}", db.join(dir).display());
    Command::new(tool)
        .args(["-o", &opt("Dir::State::Lists", "lists"), "-o", &opt("Dir::Cache", "cache")])
        .args(["-o", "Debug::NoLocking=true"])
        .args(args)
        .env("LC_ALL", "C")
        .output()
        .await
        .context(tool.to_owned())
}

async fn apt_get(db: &Path, args: &[&str]) -> Result<std::process::Output> {
    apt("apt-get", db, args).await
}

/// Lists upgrades after a refresh of the private lists in `db`, and the held-back packages (size 0).
pub async fn check(db: &Path) -> Result<(Vec<Update>, Vec<Update>)> {
    std::fs::create_dir_all(db.join("lists/partial"))?;
    std::fs::create_dir_all(db.join("cache/archives/partial"))?;
    let update = apt_get(db, &["-qq", "update"]).await?;
    if !update.status.success() {
        bail!("apt-get update: {}", String::from_utf8_lossy(&update.stderr).trim());
    }
    let sim = apt_get(db, &["-s", "dist-upgrade"]).await?;
    if !sim.status.success() {
        bail!("apt-get -s dist-upgrade: {}", String::from_utf8_lossy(&sim.stderr).trim());
    }
    let sim_text = String::from_utf8_lossy(&sim.stdout);
    let mut updates = parse_upgrades(&sim_text);
    let names = parse_held(&sim_text);
    let mut held = Vec::new();
    if !names.is_empty() {
        let mut args = vec!["policy"];
        args.extend(names.iter().map(String::as_str));
        let policy = apt("apt-cache", db, &args).await?;
        let versions = parse_policy(&String::from_utf8_lossy(&policy.stdout));
        held = names
            .into_iter()
            .map(|name| {
                let (old, new) = versions.get(&name).cloned().unwrap_or_default();
                Update { name, old, new, size: 0, ours: false }
            })
            .collect();
    }
    let uris = apt_get(db, &["-qq", "--print-uris", "dist-upgrade"]).await?;
    if uris.status.success() {
        let sizes = parse_sizes(&String::from_utf8_lossy(&uris.stdout));
        for update in &mut updates {
            update.size = sizes.get(&update.name).copied().unwrap_or(0);
        }
    }
    let ours = andromeda_update::repo::apt_names(&db.join("lists"));
    for update in &mut updates {
        update.ours = ours.contains(&update.name);
    }
    Ok((updates, held))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn upgrades_and_new_packages_from_the_simulation() {
        let text = "Calculating upgrade...\n\
            Inst apt [3.2.0] (3.2.1ubuntu0.1 Ubuntu:26.04/resolute-security [amd64])\n\
            Conf apt (3.2.1ubuntu0.1 Ubuntu:26.04/resolute-security [amd64])\n\
            Inst linux-image-7.0.0-12 (7.0.0-12.12 Ubuntu:26.04/resolute-updates [amd64])\n\
            Remv oldpkg [1.0]\n";
        let updates = parse_upgrades(text);
        assert_eq!(updates.len(), 2);
        assert_eq!((updates[0].old.as_str(), updates[0].new.as_str()), ("3.2.0", "3.2.1ubuntu0.1"));
        assert_eq!((updates[1].name.as_str(), updates[1].old.as_str()), ("linux-image-7.0.0-12", ""));
    }

    #[test]
    fn kept_back_and_phased_packages_are_held() {
        let text = "Calculating upgrade...\n\
            The following upgrades have been deferred due to phasing:\n  \
            libfoo1 libbar2\n\
            The following packages have been kept back:\n  \
            linux-generic\n\
            The following packages will be upgraded:\n  \
            apt\n\
            1 upgraded, 0 newly installed, 0 to remove and 3 not upgraded.\n";
        assert_eq!(parse_held(text), ["libfoo1", "libbar2", "linux-generic"]);
    }

    #[test]
    fn installed_and_candidate_from_policy() {
        let text = "libfoo1:\n  Installed: 1.0-1\n  Candidate: 1.1-1\n  Version table:\n     1.1-1 500\n\
            linux-generic:\n  Installed: 7.0.0.12\n  Candidate: 7.0.0.14\n";
        let policy = parse_policy(text);
        assert_eq!(policy["libfoo1"], ("1.0-1".into(), "1.1-1".into()));
        assert_eq!(policy["linux-generic"].1, "7.0.0.14");
    }

    #[test]
    fn sizes_by_package_name() {
        let text = "'http://x/apt_3.2.1_amd64.deb' apt_3.2.1_amd64.deb 1506688 MD5Sum:0890\nbroken\n";
        let sizes = parse_sizes(text);
        assert_eq!(sizes.get("apt"), Some(&1_506_688));
        assert_eq!(sizes.len(), 1);
    }
}
