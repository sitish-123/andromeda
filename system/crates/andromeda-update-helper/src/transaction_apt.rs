//! The system upgrade on apt, the equal of transaction.rs on libalpm: a package removal is a question that stops the
//! transaction, so the page shows it instead of apt taking it (docs/system.md §1). Conffile prompts keep the local
//! file. Progress comes from `APT::Status-Fd`. Pending snap refreshes run after apt, in the same authorized run.
//! Blocking: runs on its own thread.

use std::io::{BufRead, BufReader};
use std::process::{Command, Stdio};

use anyhow::{Context, Result, bail};

pub enum Event {
    /// (stage, percent 0-100, package)
    Progress(String, i32, String),
}

/// What an upgrade covers.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Scope {
    /// The full system upgrade.
    All,
    /// Only the packages of the Andromeda apt source; refused when they need a newer system package.
    Andromeda,
}

/// The message start the page shows as "update the system first".
pub const NEEDS_SYSTEM: &str = "needs a system update first";

const LISTS: &str = "/var/lib/apt/lists";

pub struct Outcome {
    /// Packages the transaction installs or upgrades ("name version"), then those it removes ("-name").
    pub targets: Vec<String>,
    /// Removals apt wants; with `committed` false, the ones that stopped the run.
    pub questions: Vec<String>,
    pub committed: bool,
}

fn apt_get() -> Command {
    let mut cmd = Command::new("apt-get");
    cmd.env("LC_ALL", "C").env("DEBIAN_FRONTEND", "noninteractive");
    // unattended-upgrades stays on (Ubuntu's daily security default); wait for its dpkg lock instead of failing.
    cmd.args(["-o", "DPkg::Lock::Timeout=600"]);
    cmd
}

/// `apt-get -s` lines: `Inst name [old] (new ...)` and `Remv name [old]`.
fn plan(text: &str) -> (Vec<String>, Vec<String>) {
    let mut targets = Vec::new();
    let mut removed = Vec::new();
    for line in text.lines() {
        let mut parts = line.split_whitespace();
        match (parts.next(), parts.next()) {
            (Some("Inst"), Some(name)) => {
                let new = parts.find_map(|p| p.strip_prefix('(')).unwrap_or_default();
                targets.push(format!("{name} {new}"));
            }
            (Some("Remv"), Some(name)) => removed.push(name.to_owned()),
            _ => {}
        }
    }
    (targets, removed)
}

/// `pmstatus:pkg:percent:text` and `dlstatus:pkg:percent:text` lines on the status fd.
fn status(line: &str) -> Option<Event> {
    let mut parts = line.splitn(4, ':');
    let stage = match parts.next()? {
        "dlstatus" => "download",
        "pmstatus" => "install",
        _ => return None,
    };
    let package = parts.next()?.to_owned();
    let percent: i32 = parts.next()?.split('.').next()?.parse().ok()?;
    Some(Event::Progress(stage.to_owned(), percent.clamp(0, 100), package))
}

/// `dry_run`: plans against the lists as they are (no refresh) and lists the targets. Otherwise refreshes, plans and
/// commits. `before_commit` runs once the commit is certain (the snapshot); its error stops the upgrade.
pub fn upgrade(
    scope: Scope,
    dry_run: bool,
    accepted: &[String],
    events: &tokio::sync::mpsc::UnboundedSender<Event>,
    before_commit: &mut dyn FnMut() -> Result<()>,
) -> Result<Outcome> {
    if scope == Scope::Andromeda {
        return upgrade_andromeda(dry_run, accepted, events, before_commit);
    }
    if !dry_run {
        let out = apt_get().args(["-qq", "update"]).output().context("apt-get update")?;
        if !out.status.success() {
            bail!("apt-get update: {}", String::from_utf8_lossy(&out.stderr).trim());
        }
    }
    let sim = apt_get().args(["-s", "dist-upgrade"]).output().context("apt-get -s dist-upgrade")?;
    if !sim.status.success() {
        bail!("apt-get -s dist-upgrade: {}", String::from_utf8_lossy(&sim.stderr).trim());
    }
    let (mut targets, removed) = plan(&String::from_utf8_lossy(&sim.stdout));
    let apt_targets = targets.len();
    let apt_installs = apt_targets > 0;
    let snaps = snap_refreshes()?;
    targets.extend(snaps.iter().cloned());
    let questions: Vec<String> = removed.iter().map(|name| format!("remove {name}")).collect();
    targets.extend(removed.iter().map(|name| format!("-{name}")));
    if dry_run || !crate::helper::consented(&questions, accepted) || targets.is_empty() {
        return Ok(Outcome { targets, questions, committed: false });
    }
    let command = if removed.is_empty() {
        vec!["dist-upgrade".to_owned()]
    } else {
        explicit(&targets[..apt_targets], &removed, accepted)?
    };
    before_commit()?;
    if apt_installs || !removed.is_empty() {
        apt_upgrade(&command, !removed.is_empty(), events)?;
    }
    if !snaps.is_empty() {
        let _ = events.send(Event::Progress("snaps".into(), 0, String::new()));
        let out = Command::new("snap").arg("refresh").env("LC_ALL", "C").output().context("snap refresh")?;
        if !out.status.success() {
            bail!("snap refresh: {}", String::from_utf8_lossy(&out.stderr).trim());
        }
    }
    Ok(Outcome { targets, questions, committed: true })
}

/// Refreshes only the Andromeda source (the other lists stay as the installed system came from them), then upgrades
/// the installed packages it offers.
fn upgrade_andromeda(
    dry_run: bool,
    accepted: &[String],
    events: &tokio::sync::mpsc::UnboundedSender<Event>,
    before_commit: &mut dyn FnMut() -> Result<()>,
) -> Result<Outcome> {
    if !dry_run {
        let source = format!("Dir::Etc::sourcelist={}", andromeda_update::repo::APT_SOURCE);
        let out = apt_get()
            .args(["-qq", "-o", &source, "-o", "Dir::Etc::sourceparts=-", "-o", "APT::Get::List-Cleanup=0", "update"])
            .output()
            .context("apt-get update")?;
        if !out.status.success() {
            bail!("apt-get update: {}", String::from_utf8_lossy(&out.stderr).trim());
        }
    }
    let ours = andromeda_update::repo::apt_names(std::path::Path::new(LISTS));
    if ours.is_empty() {
        bail!("no Andromeda apt source");
    }
    let mut args = vec!["install".to_owned(), "--only-upgrade".to_owned()];
    args.extend(ours.iter().cloned());
    let sim = apt_get().arg("-s").args(&args).output().context("apt-get -s install")?;
    if !sim.status.success() {
        let err = String::from_utf8_lossy(&sim.stderr);
        // Our packages depend on newer system packages than the installed system's lists offer.
        if String::from_utf8_lossy(&sim.stdout).contains("unmet dependencies") || err.contains("unmet dependencies") {
            bail!("{NEEDS_SYSTEM}: {}", err.trim());
        }
        bail!("apt-get -s install: {}", err.trim());
    }
    let text = String::from_utf8_lossy(&sim.stdout);
    let system = system_upgrades(&text, &ours);
    if !system.is_empty() {
        bail!("{NEEDS_SYSTEM}: {}", system.join(", "));
    }
    let (mut targets, removed) = plan(&text);
    let questions: Vec<String> = removed.iter().map(|name| format!("remove {name}")).collect();
    targets.extend(removed.iter().map(|name| format!("-{name}")));
    if dry_run || !crate::helper::consented(&questions, accepted) || targets.is_empty() {
        return Ok(Outcome { targets, questions, committed: false });
    }
    let command = if removed.is_empty() {
        pinned(&targets)
    } else {
        explicit(&targets[..targets.len() - removed.len()], &removed, accepted)?
    };
    before_commit()?;
    apt_upgrade(&command, !removed.is_empty(), events)?;
    Ok(Outcome { targets, questions, committed: true })
}

/// `install --only-upgrade` with the exact planned versions: lists that changed since the plan cannot widen the
/// install. A named package that is not installed stays out; apt brings it in as a dependency when one needs it.
fn pinned(installs: &[String]) -> Vec<String> {
    let mut args = vec!["install".to_owned(), "--only-upgrade".to_owned()];
    args.extend(installs.iter().filter_map(|t| t.split_once(' ')).map(|(name, version)| format!("{name}={version}")));
    args
}

/// The consented commit with removals: the pinned installs plus a `name-` for each planned removal. apt-get's
/// --no-remove refuses named removals too, so the commit runs without it; first a simulation of the same command
/// must remove only what the user accepted, or nothing is changed.
fn explicit(installs: &[String], removed: &[String], accepted: &[String]) -> Result<Vec<String>> {
    let mut args = pinned(installs);
    args.extend(removed.iter().map(|name| format!("{name}-")));
    let sim = apt_get().arg("-s").args(&args).output().context("apt-get -s install")?;
    if !sim.status.success() {
        bail!("apt-get -s install: {}: nothing was changed", String::from_utf8_lossy(&sim.stderr).trim());
    }
    let extra = unaccepted_removals(&String::from_utf8_lossy(&sim.stdout), accepted);
    if !extra.is_empty() {
        bail!("the update would also remove {}, which was not accepted: nothing was changed", extra.join(", "));
    }
    Ok(args)
}

/// Packages an `apt-get -s` plan removes that the user did not accept ("remove name").
fn unaccepted_removals(text: &str, accepted: &[String]) -> Vec<String> {
    let (_, removed) = plan(text);
    removed.into_iter().filter(|name| !crate::helper::accepts(accepted, &format!("remove {name}"))).collect()
}

/// Installed packages outside `ours` that an `apt-get -s` plan upgrades (`Inst name [old] ...`).
fn system_upgrades(text: &str, ours: &std::collections::HashSet<String>) -> Vec<String> {
    text.lines()
        .filter_map(|line| {
            let mut parts = line.strip_prefix("Inst ")?.split_whitespace();
            let name = parts.next()?;
            (parts.next()?.starts_with('[') && !ours.contains(name)).then(|| name.to_owned())
        })
        .collect()
}

/// "name version" for each pending snap refresh; none when snapd is not installed.
fn snap_refreshes() -> Result<Vec<String>> {
    let out = match Command::new("snap").args(["refresh", "--list"]).env("LC_ALL", "C").output() {
        Ok(out) => out,
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Ok(Vec::new()),
        Err(e) => return Err(e).context("snap refresh --list"),
    };
    if !out.status.success() {
        bail!("snap refresh --list: {}", String::from_utf8_lossy(&out.stderr).trim());
    }
    Ok(snap_targets(&String::from_utf8_lossy(&out.stdout)))
}

/// `snap refresh --list`: a header, then `name version rev size publisher notes` rows.
fn snap_targets(text: &str) -> Vec<String> {
    text.lines()
        .skip_while(|line| !line.starts_with("Name "))
        .skip(1)
        .filter_map(|line| {
            let mut cols = line.split_whitespace();
            Some(format!("{} {}", cols.next()?, cols.next()?))
        })
        .collect()
}

fn apt_upgrade(action: &[String], remove: bool, events: &tokio::sync::mpsc::UnboundedSender<Event>) -> Result<()> {
    // --no-remove: a removal that appears after the plan aborts instead of being taken. `remove`: `action` names the
    // accepted removals and a simulation of it removed nothing else (`explicit`).
    let mut child = apt_get()
        .args(["-y", "-q"])
        .args(if remove { &[][..] } else { &["--no-remove"][..] })
        .args(action)
        .args(["-o", "APT::Status-Fd=1"])
        .args(["-o", "Dpkg::Options::=--force-confdef", "-o", "Dpkg::Options::=--force-confold"])
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        // stderr goes to the journal: a second pipe nobody drains could fill and stall apt.
        .stderr(Stdio::inherit())
        .spawn()
        .context("apt-get upgrade")?;
    if let Some(stdout) = child.stdout.take() {
        for line in BufReader::new(stdout).lines().map_while(Result::ok) {
            if let Some(event) = status(&line) {
                let _ = events.send(event);
            }
        }
    }
    let code = child.wait().context("apt-get upgrade")?;
    if !code.success() {
        bail!("apt-get {} failed ({code}); see the journal of andromeda-update-helper", action[0]);
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn plan_lists_upgrades_and_removals() {
        let text = "Inst apt [3.2.0] (3.2.1ubuntu0.1 Ubuntu:26.04/resolute-security [amd64])\n\
            Conf apt (3.2.1ubuntu0.1 Ubuntu:26.04/resolute-security [amd64])\n\
            Remv oldpkg [1.0]\n";
        let (targets, removed) = plan(text);
        assert_eq!(targets, ["apt 3.2.1ubuntu0.1"]);
        assert_eq!(removed, ["oldpkg"]);
    }

    #[test]
    fn a_removal_the_user_did_not_accept_stops_the_commit() {
        let text = "Inst apt [3.2.0] (3.2.1 Ubuntu:26.04 [amd64])\nRemv oldpkg [1.0]\nRemv libgone [2.0]\n";
        let accepted = vec!["remove oldpkg".to_owned()];
        assert_eq!(unaccepted_removals(text, &accepted), ["libgone"]);
        assert!(unaccepted_removals("Remv oldpkg [1.0]\n", &accepted).is_empty());
    }

    #[test]
    fn snap_targets_skip_the_header() {
        let text = "Name     Version  Rev   Size  Publisher  Notes\nfirefox  133.0-2  5361  92MB  mozilla✓   -\n";
        assert_eq!(snap_targets(text), ["firefox 133.0-2"]);
    }

    #[test]
    fn status_lines_become_progress() {
        let Some(Event::Progress(stage, percent, package)) = status("pmstatus:apt:42.8571:Unpacking apt (amd64)")
        else {
            panic!("no event");
        };
        assert_eq!((stage.as_str(), percent, package.as_str()), ("install", 42, "apt"));
        assert!(status("Reading package lists...").is_none());
    }
}
