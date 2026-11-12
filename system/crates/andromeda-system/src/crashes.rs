//! Crashes for `per = "crash"` rules: systemd-coredump journal entries the user may read (their own; every user's
//! when in `wheel` or `systemd-journal`).

use std::collections::VecDeque;
use std::fmt::Write as _;
use std::io::{BufRead as _, Read as _, Write as _};
use std::os::unix::fs::{OpenOptionsExt as _, PermissionsExt as _};
use std::path::{Path, PathBuf};
use std::sync::Mutex;
use std::time::{Duration, Instant};

use anyhow::{Context as _, Result, bail};
use netlink_sys::{Socket, SocketAddr, protocols::NETLINK_KOBJECT_UEVENT};

use crate::crash::{iso_time, now_secs};

/// `MESSAGE_ID` of a systemd-coredump entry.
pub const COREDUMP_ID: &str = "fc2e22bc6ee647b6b90729ab34a250b1";
/// How long one crash counts in `crash.count`.
pub const WINDOW: Duration = Duration::from_secs(24 * 3600);

/// A crash this long before to this long after a desktop (compositor) crash is one of its victims: apps lose their
/// Wayland socket and dump a few seconds later, or a little before the compositor's own dump is written.
pub const AFTERMATH_BEFORE: Duration = Duration::from_secs(5);
pub const AFTERMATH: Duration = Duration::from_secs(60);

/// When the compositor last crashed; units that fail right after it are its victims too (session.rs).
static DESKTOP_CRASHED_AT: Mutex<Option<Instant>> = Mutex::new(None);

pub fn desktop_crashed_at() -> Option<Instant> {
    *DESKTOP_CRASHED_AT.lock().unwrap_or_else(std::sync::PoisonError::into_inner)
}

/// The Andromeda compositor's executable, whatever its install prefix.
pub fn is_desktop(exe: &str) -> bool {
    exe.rsplit('/').next() == Some("andromeda")
}

/// The executable of one `journalctl -o json` coredump line: `COREDUMP_EXE`, else `COREDUMP_COMM`.
pub fn exe_of(line: &str) -> Option<String> {
    let entry: serde_json::Value = serde_json::from_str(line).ok()?;
    if entry.get("MESSAGE_ID")?.as_str()? != COREDUMP_ID {
        return None;
    }
    // si_code 0 is SI_USER: another process sent the signal with kill(), not a fault.
    if entry.get("COREDUMP_CODE").and_then(serde_json::Value::as_str) == Some("0") {
        return None;
    }
    let field = |k: &str| entry.get(k).and_then(serde_json::Value::as_str).filter(|s| !s.is_empty());
    field("COREDUMP_EXE").or_else(|| field("COREDUMP_COMM")).map(str::to_owned)
}

/// Crashes of the last `WINDOW`, oldest first.
#[derive(Default)]
pub struct Recent(VecDeque<(String, Instant)>);

impl Recent {
    pub fn add(&mut self, exe: String, at: Instant) {
        if is_desktop(&exe) {
            *DESKTOP_CRASHED_AT.lock().unwrap_or_else(std::sync::PoisonError::into_inner) = Some(at);
        }
        self.0.push_back((exe, at));
    }

    /// Executable -> (crashes in the window at `now`, every one of them came with a desktop crash); older ones are
    /// dropped.
    pub fn counts(&mut self, now: Instant) -> Vec<(String, usize, bool)> {
        while self.0.front().is_some_and(|(_, at)| now.duration_since(*at) >= WINDOW) {
            self.0.pop_front();
        }
        let desktop: Vec<Instant> = self.0.iter().filter(|(e, _)| is_desktop(e)).map(|(_, at)| *at).collect();
        let victim = |exe: &str, at: Instant| {
            !is_desktop(exe)
                && desktop.iter().any(|d| {
                    at.checked_duration_since(*d)
                        .map_or_else(|| d.duration_since(at) <= AFTERMATH_BEFORE, |after| after <= AFTERMATH)
                })
        };
        let mut counts: Vec<(String, usize, bool)> = Vec::new();
        for (exe, at) in &self.0 {
            let with_desktop = victim(exe, *at);
            match counts.iter_mut().find(|(e, _, _)| e == exe) {
                Some((_, n, all)) => {
                    *n += 1;
                    *all &= with_desktop;
                }
                None => counts.push((exe.clone(), 1, with_desktop)),
            }
        }
        counts
    }
}

/// Device coredumps (GPU hangs and resets); the kernel frees each one about 5 minutes after it appears.
const DEVCOREDUMP_CLASS: &str = "/sys/class/devcoredump";
/// A larger dump is left in sysfs: the daemon has a 64M memory cap and the store sits on `/var`.
pub const DUMP_MAX: u64 = 64 << 20;
const HEAD_LINES: usize = 20;

/// Saves the dumps already present, then one per kernel `add` uevent of subsystem devcoredump, on its own thread.
pub fn watch_devcoredumps(store: PathBuf) {
    let socket = (|| -> Result<Socket> {
        let mut socket = Socket::new(NETLINK_KOBJECT_UEVENT).context("uevent socket")?;
        socket.bind(&SocketAddr::new(0, 1)).context("uevent bind")?;
        Ok(socket)
    })();
    let spawned = std::thread::Builder::new().name("devcoredump".into()).spawn(move || {
        let scan = || {
            if let Ok(entries) = std::fs::read_dir(DEVCOREDUMP_CLASS) {
                for entry in entries.flatten() {
                    if entry.file_name().to_string_lossy().starts_with("devcd") {
                        save_logged(&entry.path(), &store);
                    }
                }
            }
        };
        scan();
        let socket = match socket {
            Ok(socket) => socket,
            Err(e) => return log::warn!("devcoredump: no uevents ({e:#}); dumps are not saved"),
        };
        loop {
            match socket.recv_from_full() {
                Ok((msg, from)) => {
                    if from.port_number() == 0
                        && let Some(name) = devcoredump_added(&msg)
                    {
                        save_logged(&Path::new(DEVCOREDUMP_CLASS).join(name), &store);
                    }
                }
                Err(e) if e.kind() == std::io::ErrorKind::Interrupted => {}
                Err(e) if e.raw_os_error() == Some(rustix::io::Errno::NOBUFS.raw_os_error()) => {
                    log::warn!("devcoredump: uevents dropped (receive buffer full); rescanning");
                    scan();
                }
                Err(e) => return log::warn!("devcoredump: uevent socket: {e}; dumps are not saved"),
            }
        }
    });
    if let Err(e) = spawned {
        log::warn!("devcoredump: {e}");
    }
}

fn save_logged(dev: &Path, store: &Path) {
    match save_devcoredump(dev, store, now_secs(), DUMP_MAX) {
        Ok(Saved::Report(report)) => log::warn!("devcoredump: saved {}", report.display()),
        Ok(Saved::TooLarge) => log::warn!("devcoredump: {} is over {DUMP_MAX} bytes; left in sysfs", dev.display()),
        Ok(Saved::Already) => {}
        Err(e) => log::warn!("devcoredump: {}: {e:#}", dev.display()),
    }
}

/// The `devcdN` name of a kernel uevent that adds a devcoredump.
fn devcoredump_added(msg: &[u8]) -> Option<&str> {
    if !msg.starts_with(b"add@/devices/virtual/devcoredump/") {
        return None;
    }
    let mut fields = msg.split(|&b| b == 0).filter_map(|f| std::str::from_utf8(f).ok());
    let header = fields.next()?;
    let fields: Vec<&str> = fields.collect();
    if !header.starts_with("add@") || !fields.contains(&"ACTION=add") || !fields.contains(&"SUBSYSTEM=devcoredump") {
        return None;
    }
    let name = header.rsplit('/').next()?;
    let n = name.strip_prefix("devcd")?;
    (!n.is_empty() && n.bytes().all(|b| b.is_ascii_digit())).then_some(name)
}

#[derive(Debug, PartialEq, Eq)]
pub enum Saved {
    Report(PathBuf),
    TooLarge,
    /// An earlier daemon run saved it: the kernel keeps a dump until its timeout, across daemon restarts.
    Already,
}

/// Bytes at the start of a dump that, with its failing device, identify it among the dumps already saved.
const ID_BYTES: u64 = 4096;

/// Copies the dump of `dev` (a `devcdN` sysfs dir) to `<store>/devcoredump-<time>-<devcdN>.dump` and writes the
/// `.txt` report that `andromeda debug crashes` lists. sysfs is read-only to the daemon: the kernel frees the dump.
pub fn save_devcoredump(dev: &Path, store: &Path, time: u64, max: u64) -> Result<Saved> {
    let name = dev.file_name().context("no devcd name")?.to_string_lossy().into_owned();
    let link_name = |p: PathBuf| std::fs::read_link(p).ok()?.file_name().map(|n| n.to_string_lossy().into_owned());
    let failing = link_name(dev.join("failing_device")).unwrap_or_else(|| "?".to_owned());
    let device = match link_name(dev.join("failing_device/driver")) {
        Some(driver) => format!("{failing} ({driver})"),
        None => failing,
    };
    let mut data = std::fs::File::open(dev.join("data")).context("open data")?;
    let mut id = Vec::new();
    (&mut data).take(ID_BYTES).read_to_end(&mut id).context("read data")?;
    if id.is_empty() {
        bail!("empty dump");
    }
    std::fs::create_dir_all(store).context("crash store")?;
    if already_saved(store, &name, &device, &id) {
        return Ok(Saved::Already);
    }
    let stem = store.join(format!("devcoredump-{}-{name}", iso_time(time, true)));
    let dump = stem.with_extension("dump");
    let copy = || -> Result<u64> {
        // 0600: a dump can hold GPU memory of any user; the .txt report stays readable for `andromeda debug crashes`.
        let mut out = std::fs::OpenOptions::new()
            .write(true)
            .create(true)
            .truncate(true)
            .mode(0o600)
            .open(&dump)
            .context("create dump")?;
        out.set_permissions(std::fs::Permissions::from_mode(0o600)).context("chmod dump")?;
        out.write_all(&id).context("write dump")?;
        let rest = (max + 1).saturating_sub(id.len() as u64);
        Ok(id.len() as u64 + std::io::copy(&mut data.take(rest), &mut out).context("copy data")?)
    };
    let copied = copy().inspect_err(|_| {
        let _ = std::fs::remove_file(&dump);
    })?;
    if copied > max {
        std::fs::remove_file(&dump)?;
        return Ok(Saved::TooLarge);
    }
    let head: Vec<String> = std::io::BufReader::new(std::fs::File::open(&dump)?.take(16 << 10))
        .split(b'\n')
        .map_while(std::result::Result::ok)
        .map(|l| String::from_utf8_lossy(&l).trim_end().chars().take(200).collect::<String>())
        .filter(|l| !l.is_empty() && l.chars().all(|c| c == '\t' || !c.is_control() && c != '\u{FFFD}'))
        .take(HEAD_LINES)
        .collect();
    let mut report = String::new();
    let _ = writeln!(report, "component: devcoredump");
    let _ = writeln!(report, "signal: devcoredump");
    let _ = writeln!(report, "time: {}", iso_time(time, false));
    let _ = writeln!(report, "message: {}", head.iter().take(3).cloned().collect::<Vec<_>>().join(" | "));
    let _ = writeln!(report, "location: {device}");
    let _ = writeln!(report, "dump: {} ({copied} bytes)", dump.display());
    report.push_str("log:\n");
    for line in &head {
        let _ = writeln!(report, "{line}");
    }
    let path = stem.with_extension("txt");
    std::fs::write(&path, report).context("write report")?;
    prune(store, KEEP_DUMPS);
    Ok(Saved::Report(path))
}

const KEEP_DUMPS: usize = 5;

/// Deletes all but the newest `keep` devcoredump dumps and their reports; file names sort by time.
fn prune(store: &Path, keep: usize) {
    let Ok(entries) = std::fs::read_dir(store) else { return };
    let mut dumps: Vec<PathBuf> = entries
        .flatten()
        .map(|e| e.path())
        .filter(|p| {
            let name = p.file_name().unwrap_or_default().to_string_lossy();
            name.starts_with("devcoredump-") && name.ends_with(".dump")
        })
        .collect();
    dumps.sort();
    for old in dumps.iter().rev().skip(keep) {
        let _ = std::fs::remove_file(old);
        let _ = std::fs::remove_file(old.with_extension("txt"));
    }
}

/// A saved `<...>-<name>.dump` starts with `id` and its report names `device`.
fn already_saved(store: &Path, name: &str, device: &str, id: &[u8]) -> bool {
    let Ok(entries) = std::fs::read_dir(store) else { return false };
    let suffix = format!("-{name}.dump");
    let location = format!("\nlocation: {device}\n");
    entries.flatten().any(|e| {
        let path = e.path();
        if !e.file_name().to_string_lossy().ends_with(&suffix) {
            return false;
        }
        let mut start = Vec::new();
        let read = std::fs::File::open(&path).and_then(|f| f.take(id.len() as u64).read_to_end(&mut start));
        read.is_ok()
            && start == id
            && std::fs::read_to_string(path.with_extension("txt")).is_ok_and(|t| t.contains(&location))
    })
}

#[cfg(test)]
#[allow(clippy::unwrap_used)]
mod tests {
    use super::*;

    fn fake_devcd(root: &Path, dump: &str) -> PathBuf {
        let _ = std::fs::remove_dir_all(root);
        let pci = root.join("devices/0000:00:02.0");
        std::fs::create_dir_all(&pci).unwrap();
        std::fs::create_dir_all(root.join("devices/xe")).unwrap();
        std::os::unix::fs::symlink(root.join("devices/xe"), pci.join("driver")).unwrap();
        let dev = root.join("class/devcd3");
        std::fs::create_dir_all(&dev).unwrap();
        std::fs::write(dev.join("data"), dump).unwrap();
        std::os::unix::fs::symlink(&pci, dev.join("failing_device")).unwrap();
        dev
    }

    #[test]
    fn a_devcoredump_is_copied_and_listed_once() {
        let root = std::env::temp_dir().join(format!("devcd-saved-{}", std::process::id()));
        let dump = "**** Xe Device Coredump ****\n\nreason: Timedout job\nkernel: 7.3.1\n\x01\x02binary tail";
        let dev = fake_devcd(&root, dump);
        let store = root.join("crashes");
        let Saved::Report(report) = save_devcoredump(&dev, &store, 1_794_478_500, DUMP_MAX).unwrap() else {
            panic!("not saved")
        };
        assert_eq!(report, store.join("devcoredump-20261112T101500Z-devcd3.txt"));
        assert_eq!(std::fs::read_to_string(store.join("devcoredump-20261112T101500Z-devcd3.dump")).unwrap(), dump);
        let text = std::fs::read_to_string(&report).unwrap();
        assert!(
            text.starts_with("component: devcoredump\nsignal: devcoredump\ntime: 2026-11-12T10:15:00Z\n"),
            "{text}"
        );
        assert!(
            text.contains("\nmessage: **** Xe Device Coredump **** | reason: Timedout job | kernel: 7.3.1\n"),
            "{text}"
        );
        assert!(text.contains("\nlocation: 0000:00:02.0 (xe)\n"), "{text}");
        assert!(text.contains("\nlog:\n**** Xe Device Coredump ****\nreason: Timedout job\n"), "{text}");
        assert!(!text.contains("binary tail"), "{text}");
        let mode =
            std::fs::metadata(store.join("devcoredump-20261112T101500Z-devcd3.dump")).unwrap().permissions().mode();
        assert_eq!(mode & 0o777, 0o600);
        assert_eq!(save_devcoredump(&dev, &store, 1_794_478_560, DUMP_MAX).unwrap(), Saved::Already);
        assert_eq!(std::fs::read_dir(&store).unwrap().count(), 2);
        std::fs::remove_dir_all(&root).unwrap();
    }

    #[test]
    fn a_devcoredump_over_the_cap_is_left_in_sysfs() {
        let root = std::env::temp_dir().join(format!("devcd-big-{}", std::process::id()));
        let dev = fake_devcd(&root, "0123456789");
        let store = root.join("crashes");
        assert_eq!(save_devcoredump(&dev, &store, 0, 9).unwrap(), Saved::TooLarge);
        assert_eq!(std::fs::read_dir(&store).unwrap().count(), 0);
        assert_eq!(std::fs::read_to_string(dev.join("data")).unwrap(), "0123456789");
        std::fs::remove_dir_all(&root).unwrap();
    }

    #[test]
    fn a_failed_copy_leaves_no_dump() {
        let root = std::env::temp_dir().join(format!("devcd-full-{}", std::process::id()));
        let dev = fake_devcd(&root, "dump");
        let store = root.join("crashes");
        std::fs::create_dir_all(&store).unwrap();
        let dump = store.join("devcoredump-19700101T000000Z-devcd3.dump");
        std::os::unix::fs::symlink("/dev/full", &dump).unwrap();
        assert!(save_devcoredump(&dev, &store, 0, DUMP_MAX).is_err());
        assert!(std::fs::symlink_metadata(&dump).is_err());
        std::fs::remove_dir_all(&root).unwrap();
    }

    #[test]
    fn only_the_newest_dumps_are_kept() {
        let root = std::env::temp_dir().join(format!("devcd-keep-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&root);
        std::fs::create_dir_all(&root).unwrap();
        let stems: Vec<String> = (0..7).map(|i| format!("devcoredump-2026111{i}T000000Z-devcd{i}")).collect();
        for stem in &stems {
            std::fs::write(root.join(format!("{stem}.dump")), "d").unwrap();
            std::fs::write(root.join(format!("{stem}.txt")), "t").unwrap();
        }
        std::fs::write(root.join("andromeda-20200101T000000Z-1.txt"), "t").unwrap();
        prune(&root, 5);
        let mut left: Vec<String> =
            std::fs::read_dir(&root).unwrap().map(|e| e.unwrap().file_name().to_string_lossy().into_owned()).collect();
        left.sort();
        let mut want = vec!["andromeda-20200101T000000Z-1.txt".to_owned()];
        want.extend(stems[2..].iter().flat_map(|s| [format!("{s}.dump"), format!("{s}.txt")]));
        assert_eq!(left, want);
        std::fs::remove_dir_all(&root).unwrap();
    }

    #[test]
    fn only_a_devcoredump_add_uevent_names_a_dump() {
        let add = b"add@/devices/virtual/devcoredump/devcd1\0ACTION=add\0DEVPATH=/devices/virtual/devcoredump/devcd1\0SUBSYSTEM=devcoredump\0";
        assert_eq!(devcoredump_added(add), Some("devcd1"));
        let remove = b"remove@/devices/virtual/devcoredump/devcd1\0ACTION=remove\0SUBSYSTEM=devcoredump\0";
        assert_eq!(devcoredump_added(remove), None);
        let other = b"add@/devices/virtual/devcoredump/../x\0ACTION=add\0SUBSYSTEM=devcoredump\0";
        assert_eq!(devcoredump_added(other), None);
    }

    #[test]
    fn a_coredump_line_names_its_executable_and_other_lines_do_not() {
        let line = format!(r#"{{"MESSAGE_ID":"{COREDUMP_ID}","COREDUMP_EXE":"/usr/bin/foot","COREDUMP_COMM":"foot"}}"#);
        assert_eq!(exe_of(&line).as_deref(), Some("/usr/bin/foot"));
        assert_eq!(exe_of(r#"{"MESSAGE_ID":"x","COREDUMP_EXE":"/usr/bin/foot"}"#), None);
    }

    #[test]
    fn a_signal_sent_by_kill_is_not_a_crash() {
        let line = |code| {
            format!(r#"{{"MESSAGE_ID":"{COREDUMP_ID}","COREDUMP_EXE":"/usr/bin/foot","COREDUMP_CODE":"{code}"}}"#)
        };
        assert_eq!(exe_of(&line(0)), None);
        assert_eq!(exe_of(&line(1)).as_deref(), Some("/usr/bin/foot"));
    }

    #[test]
    fn a_crash_counts_for_one_window() {
        let t0 = Instant::now();
        let mut recent = Recent::default();
        recent.add("/a".into(), t0);
        recent.add("/a".into(), t0 + Duration::from_secs(60));
        assert_eq!(recent.counts(t0 + Duration::from_secs(61)), [("/a".to_owned(), 2, false)]);
        assert_eq!(recent.counts(t0 + WINDOW + Duration::from_secs(1)), [("/a".to_owned(), 1, false)]);
    }

    #[test]
    fn apps_that_die_with_the_compositor_are_its_victims_and_later_crashes_are_not() {
        let t0 = Instant::now();
        let mut recent = Recent::default();
        recent.add("/usr/bin/andromeda".into(), t0);
        recent.add("/opt/zen-browser-bin/zen-bin".into(), t0 + Duration::from_secs(4));
        recent.add("/usr/bin/foot".into(), t0 + Duration::from_secs(600));
        let counts = recent.counts(t0 + Duration::from_secs(601));
        let with_desktop = |exe: &str| counts.iter().find(|(e, _, _)| e == exe).map(|(_, _, w)| *w);
        assert_eq!(with_desktop("/usr/bin/andromeda"), Some(false));
        assert_eq!(with_desktop("/opt/zen-browser-bin/zen-bin"), Some(true));
        assert_eq!(with_desktop("/usr/bin/foot"), Some(false));
    }
}
