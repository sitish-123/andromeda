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

pub struct Outcome {
    /// Packages the transaction installs or upgrades ("name version"), then those it removes ("-name").
    pub targets: Vec<String>,
    /// Removals apt wants; non-empty means nothing was committed.
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
    dry_run: bool,
    events: &tokio::sync::mpsc::UnboundedSender<Event>,
    before_commit: &mut dyn FnMut() -> Result<()>,
) -> Result<Outcome> {
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
    let apt_installs = !targets.is_empty();
    let snaps = snap_refreshes()?;
    targets.extend(snaps.iter().cloned());
    let questions: Vec<String> = removed.iter().map(|name| format!("remove {name}")).collect();
    targets.extend(removed.iter().map(|name| format!("-{name}")));
    if dry_run || !questions.is_empty() || targets.is_empty() {
        return Ok(Outcome { targets, questions, committed: false });
    }
    before_commit()?;
    if apt_installs {
        apt_upgrade(events)?;
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

fn apt_upgrade(events: &tokio::sync::mpsc::UnboundedSender<Event>) -> Result<()> {
    // --no-remove: a removal that appears after the plan aborts instead of being taken.
    let mut child = apt_get()
        .args(["-y", "-q", "--no-remove", "dist-upgrade", "-o", "APT::Status-Fd=1"])
        .args(["-o", "Dpkg::Options::=--force-confdef", "-o", "Dpkg::Options::=--force-confold"])
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        // stderr goes to the journal: a second pipe nobody drains could fill and stall apt.
        .stderr(Stdio::inherit())
        .spawn()
        .context("apt-get dist-upgrade")?;
    if let Some(stdout) = child.stdout.take() {
        for line in BufReader::new(stdout).lines().map_while(Result::ok) {
            if let Some(event) = status(&line) {
                let _ = events.send(event);
            }
        }
    }
    let code = child.wait().context("apt-get dist-upgrade")?;
    if !code.success() {
        bail!("apt-get dist-upgrade failed ({code}); see the journal of andromeda-update-helper");
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
