//! System migrations: one-shot scripts the package ships in `/usr/share/andromeda/migrations`, run once each, in name
//! order, as root. A pacman hook runs them after every transaction that brings in a script (so a plain `pacman -Syu`
//! runs them too), and `Upgrade` runs any still pending. Names that ran are kept in `<state dir>/migrations-done`,
//! which the package's install script seeds at install time. A failed script stops the run and is not recorded, so
//! the next run retries it.

use std::fs::{self, File, OpenOptions};
use std::io::{ErrorKind, Write};
use std::os::unix::fs::{MetadataExt, PermissionsExt};
use std::os::unix::process::CommandExt;
use std::path::Path;
use std::process::{Command, Stdio};
use std::time::{Duration, Instant};

use anyhow::{Context, Result, bail};
use rustix::fs::{FlockOperation, flock};
use rustix::process::{Pid, Signal, kill_process_group};

pub const DIR: &str = "/usr/share/andromeda/migrations";
const DONE: &str = "migrations-done";
const LOCK: &str = "migrations.lock";
const TIMEOUT: Duration = Duration::from_secs(300);

/// Owned by root (when `require_root`) and writable by nobody else.
fn trusted(meta: &fs::Metadata, require_root: bool) -> bool {
    meta.mode() & 0o022 == 0 && (!require_root || meta.uid() == 0)
}

/// Executable regular files (symlinks skipped), by name. The directory itself must be trusted, or a user who can
/// write it could swap a script between this check and its exec.
fn scripts(dir: &Path, require_root: bool) -> Result<Vec<String>> {
    match fs::symlink_metadata(dir) {
        Err(e) if e.kind() == ErrorKind::NotFound => return Ok(Vec::new()),
        Err(e) => return Err(e).context(dir.display().to_string()),
        Ok(meta) if !meta.is_dir() || !trusted(&meta, require_root) => {
            bail!("{} is not a root-only directory", dir.display())
        }
        Ok(_) => {}
    }
    let mut names = Vec::new();
    for entry in fs::read_dir(dir)? {
        let entry = entry?;
        let meta = entry.metadata()?;
        if meta.is_file() && meta.permissions().mode() & 0o111 != 0 && trusted(&meta, require_root) {
            names.extend(entry.file_name().into_string().ok());
        }
    }
    names.sort();
    Ok(names)
}

fn done(state_dir: &Path) -> Result<Vec<String>> {
    let text = fs::read_to_string(state_dir.join(DONE))
        .context("the migration list is missing or unreadable; reinstall andromeda-system-git to seed it")?;
    Ok(text.lines().map(str::to_owned).collect())
}

fn record(state_dir: &Path, name: &str) -> Result<()> {
    let mut file = OpenOptions::new().append(true).open(state_dir.join(DONE))?;
    writeln!(file, "{name}")?;
    file.sync_all()?;
    Ok(())
}

/// Serialises the pacman hook and the helper.
fn lock(state_dir: &Path) -> Result<File> {
    fs::create_dir_all(state_dir)?;
    let file = OpenOptions::new().create(true).truncate(false).write(true).open(state_dir.join(LOCK))?;
    flock(&file, FlockOperation::LockExclusive)?;
    Ok(file)
}

/// Install time: the scripts already present describe changes this install already has, so they are recorded as done.
/// Does nothing when the list exists.
pub fn seed(dir: &Path, state_dir: &Path, require_root: bool) -> Result<()> {
    let _lock = lock(state_dir)?;
    let target = state_dir.join(DONE);
    if target.exists() {
        return Ok(());
    }
    let tmp = state_dir.join(format!("{DONE}.new"));
    let mut file = File::create(&tmp)?;
    for name in scripts(dir, require_root)? {
        writeln!(file, "{name}")?;
    }
    file.sync_all()?;
    fs::rename(&tmp, &target)?;
    File::open(state_dir)?.sync_all()?;
    Ok(())
}

/// Runs the scripts not done yet; returns their names. Stops at the first failure.
pub fn run(dir: &Path, state_dir: &Path, require_root: bool) -> Result<Vec<String>> {
    let _lock = lock(state_dir)?;
    let done = done(state_dir)?;
    let mut ran = Vec::new();
    for name in scripts(dir, require_root)?.into_iter().filter(|n| !done.contains(n)) {
        log::info!("migration {name}");
        // Own process group, so a timeout kills what the script started too.
        let mut child = Command::new(dir.join(&name))
            .env_clear()
            .env("PATH", "/usr/bin")
            .env("LANG", "C.UTF-8")
            .stdin(Stdio::null())
            .process_group(0)
            .spawn()
            .with_context(|| format!("migration {name}"))?;
        let started = Instant::now();
        let status = loop {
            if let Some(status) = child.try_wait()? {
                break status;
            }
            if started.elapsed() > TIMEOUT {
                if let Some(pid) = i32::try_from(child.id()).ok().and_then(Pid::from_raw) {
                    let _ = kill_process_group(pid, Signal::KILL);
                }
                let _ = child.wait();
                bail!("migration {name} did not finish in {} s", TIMEOUT.as_secs());
            }
            std::thread::sleep(Duration::from_millis(100));
        };
        if !status.success() {
            bail!("migration {name} failed ({status})");
        }
        record(state_dir, &name)?;
        ran.push(name);
    }
    Ok(ran)
}

#[cfg(test)]
#[allow(clippy::unwrap_used)]
mod tests {
    use super::*;

    fn script(dir: &Path, name: &str, body: &str) {
        let path = dir.join(name);
        fs::write(&path, format!("#!/bin/sh\n{body}\n")).unwrap();
        fs::set_permissions(&path, fs::Permissions::from_mode(0o755)).unwrap();
    }

    #[test]
    fn runs_new_scripts_once_in_order_and_retries_a_failure() {
        let tmp = tempfile::tempdir().unwrap();
        let (dir, state, out) = (tmp.path().join("m"), tmp.path().join("s"), tmp.path().join("out"));
        fs::create_dir_all(&dir).unwrap();
        fs::set_permissions(&dir, fs::Permissions::from_mode(0o755)).unwrap();
        assert!(run(&dir, &state, false).is_err(), "an unseeded install runs nothing");
        script(&dir, "001-old", &format!("echo old >> {}", out.display()));
        seed(&dir, &state, false).unwrap();
        script(&dir, "003-third", &format!("echo third >> {}", out.display()));
        script(&dir, "002-second", &format!("echo second >> {}; exit 1", out.display()));
        seed(&dir, &state, false).unwrap();

        assert!(run(&dir, &state, false).is_err());
        script(&dir, "002-second", &format!("echo second >> {}", out.display()));
        assert_eq!(run(&dir, &state, false).unwrap(), ["002-second", "003-third"]);
        assert!(run(&dir, &state, false).unwrap().is_empty());
        assert_eq!(fs::read_to_string(&out).unwrap(), "second\nsecond\nthird\n");
    }

    #[test]
    fn refuses_a_group_writable_directory() {
        let tmp = tempfile::tempdir().unwrap();
        let (dir, state) = (tmp.path().join("m"), tmp.path().join("s"));
        fs::create_dir_all(&dir).unwrap();
        fs::set_permissions(&dir, fs::Permissions::from_mode(0o775)).unwrap();
        assert!(seed(&dir, &state, false).is_err());
    }
}
