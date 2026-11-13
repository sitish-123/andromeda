//! Crash capture: a ring of the last log lines and a panic hook that writes the report format of the C++ handler
//! (`protocol/crash_capture.h`) to `<dir>/<component>-<UTC time>-<pid>.txt`, read by `andromeda debug crashes`. The
//! system service writes to `$STATE_DIRECTORY/crashes`; a user process to `$XDG_STATE_HOME/andromeda/crashes`.

use std::fmt::Write as _;
use std::io::Write as _;
use std::os::unix::fs::MetadataExt as _;
use std::panic::PanicHookInfo;
use std::path::PathBuf;
use std::sync::{Mutex, TryLockError};
use std::time::{SystemTime, UNIX_EPOCH};

const RING_LINES: usize = 200;

/// The last log lines; each slot's `String` is reused, so a steady state allocates nothing per line.
struct Ring {
    lines: Vec<String>,
    next: usize,
}

static RING: Mutex<Ring> = Mutex::new(Ring { lines: Vec::new(), next: 0 });

impl Ring {
    fn push(&mut self, line: &str) {
        if self.lines.len() < RING_LINES {
            self.lines.push(String::new());
        }
        let slot = &mut self.lines[self.next % RING_LINES];
        slot.clear();
        slot.push_str(line);
        self.next += 1;
    }

    /// Oldest first.
    fn ordered(&self) -> Vec<String> {
        let start = if self.lines.len() < RING_LINES { 0 } else { self.next % RING_LINES };
        (0..self.lines.len()).map(|i| self.lines[(start + i) % self.lines.len()].clone()).collect()
    }
}

struct RingLogger {
    inner: env_logger::Logger,
}

impl log::Log for RingLogger {
    fn enabled(&self, metadata: &log::Metadata<'_>) -> bool {
        self.inner.enabled(metadata)
    }

    fn log(&self, record: &log::Record<'_>) {
        if !self.inner.matches(record) {
            return;
        }
        self.inner.log(record);
        record_line(record.level(), record.target(), *record.args());
    }

    fn flush(&self) {
        self.inner.flush();
    }
}

/// Formats outside the lock, so a panic in a `Display` impl cannot happen while the ring is held. A logger never
/// blocks on the ring: a line logged while another thread holds it is dropped from the ring.
fn record_line(level: log::Level, target: &str, args: std::fmt::Arguments<'_>) {
    let secs = now_secs() % 86400;
    let line =
        format!("{:02}:{:02}:{:02} {} [{target}] {args}", secs / 3600, secs / 60 % 60, secs % 60, &level.as_str()[..1]);
    if let Ok(mut ring) = RING.try_lock() {
        ring.push(&line);
    }
}

/// The ring's lines, oldest first; None when another thread holds it (the panic may have hit while it did).
fn ring_snapshot() -> Option<Vec<String>> {
    match RING.try_lock() {
        Ok(ring) => Some(ring.ordered()),
        Err(TryLockError::Poisoned(poisoned)) => Some(poisoned.into_inner().ordered()),
        Err(TryLockError::WouldBlock) => None,
    }
}

/// The sd-daemon level prefix when stderr is the journal stream (`$JOURNAL_STREAM` is its "dev:inode"), so journald
/// stores the line's real priority instead of 6.
fn stderr_is_journal() -> bool {
    let Some(stream) = std::env::var_os("JOURNAL_STREAM") else { return false };
    std::fs::metadata("/proc/self/fd/2").is_ok_and(|m| stream.to_str() == Some(&format!("{}:{}", m.dev(), m.ino())))
}

/// Installs `env_logger` (`RUST_LOG`, default info) behind the ring, and the panic hook.
pub fn init(component: &'static str) {
    let mut builder = env_logger::Builder::from_env(env_logger::Env::default().default_filter_or("info"));
    if stderr_is_journal() {
        builder.format(|buf, record| {
            let priority = match record.level() {
                log::Level::Error => 3,
                log::Level::Warn => 4,
                log::Level::Info => 6,
                log::Level::Debug | log::Level::Trace => 7,
            };
            writeln!(buf, "<{priority}>[{} {}] {}", record.level(), record.target(), record.args())
        });
    }
    let inner = builder.build();
    let filter = inner.filter();
    if log::set_boxed_logger(Box::new(RingLogger { inner })).is_ok() {
        log::set_max_level(filter);
    }
    let previous = std::panic::take_hook();
    std::panic::set_hook(Box::new(move |info| {
        write_report(component, info);
        previous(info);
    }));
}

fn crash_dir() -> Option<PathBuf> {
    if let Some(state) = std::env::var_os("STATE_DIRECTORY") {
        return Some(PathBuf::from(state).join("crashes"));
    }
    if let Some(state) = std::env::var_os("XDG_STATE_HOME").filter(|s| !s.is_empty()) {
        return Some(PathBuf::from(state).join("andromeda/crashes"));
    }
    std::env::var_os("HOME").filter(|s| !s.is_empty()).map(|h| PathBuf::from(h).join(".local/state/andromeda/crashes"))
}

fn write_report(component: &str, info: &PanicHookInfo<'_>) {
    let Some(dir) = crash_dir() else { return };
    let message = info
        .payload()
        .downcast_ref::<&str>()
        .map(|s| (*s).to_owned())
        .or_else(|| info.payload().downcast_ref::<String>().cloned())
        .unwrap_or_else(|| "<non-string panic payload>".to_owned());
    let location =
        info.location().map_or_else(|| "?".to_owned(), |l| format!("{}:{}:{}", l.file(), l.line(), l.column()));
    let time = now_secs();
    let pid = std::process::id();
    let path = dir.join(format!("{component}-{}-{pid}.txt", iso_time(time, true)));
    let text = build_report(component, time, &message, &location);
    if std::fs::create_dir_all(&dir).and_then(|()| std::fs::write(&path, text)).is_ok() {
        eprintln!("{component}: panic, crash report: {}", path.display());
    }
}

fn build_report(component: &str, time: u64, message: &str, location: &str) -> String {
    let backtrace = std::backtrace::Backtrace::force_capture().to_string();
    let log = ring_snapshot().unwrap_or_else(|| vec!["(log ring held by another thread; lines omitted)".to_owned()]);
    let report =
        Report { component, version: env!("CARGO_PKG_VERSION"), pid: std::process::id(), time, message, location };
    report.format(&backtrace, &log)
}

struct Report<'a> {
    component: &'a str,
    version: &'a str,
    pid: u32,
    time: u64,
    message: &'a str,
    location: &'a str,
}

impl Report<'_> {
    fn format(&self, backtrace: &str, log: &[String]) -> String {
        let mut out = String::new();
        let _ = writeln!(out, "component: {}", self.component);
        let _ = writeln!(out, "version: {}", self.version);
        let _ = writeln!(out, "pid: {}", self.pid);
        let _ = writeln!(out, "signal: panic");
        let _ = writeln!(out, "time: {}", iso_time(self.time, false));
        let _ = writeln!(out, "message: {}", self.message.replace('\n', " "));
        let _ = writeln!(out, "location: {}", self.location);
        out.push_str("backtrace:\n");
        for line in backtrace.lines() {
            let _ = writeln!(out, "{line}");
        }
        out.push_str("log:\n");
        for line in log {
            let _ = writeln!(out, "{line}");
        }
        out
    }
}

pub fn now_secs() -> u64 {
    SystemTime::now().duration_since(UNIX_EPOCH).map_or(0, |d| d.as_secs())
}

/// UTC; `basic` is 20261112T101500Z (file names), else 2026-11-12T10:15:00Z.
pub fn iso_time(secs: u64, basic: bool) -> String {
    // Days since 1970-01-01 to a civil date (Howard Hinnant's algorithm).
    let days = i64::try_from(secs / 86400).unwrap_or(0) + 719_468;
    let era = days.div_euclid(146_097);
    let doe = days - era * 146_097;
    let yoe = (doe - doe / 1460 + doe / 36524 - doe / 146_096) / 365;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let day = doy - (153 * mp + 2) / 5 + 1;
    let month = if mp < 10 { mp + 3 } else { mp - 9 };
    let year = yoe + era * 400 + i64::from(month <= 2);
    let tod = secs % 86400;
    let (hh, mm, ss) = (tod / 3600, tod / 60 % 60, tod % 60);
    if basic {
        format!("{year:04}{month:02}{day:02}T{hh:02}{mm:02}{ss:02}Z")
    } else {
        format!("{year:04}-{month:02}-{day:02}T{hh:02}:{mm:02}:{ss:02}Z")
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn report_has_the_crash_file_fields_in_order() {
        let report = Report {
            component: "andromeda-system",
            version: "1.2.3",
            pid: 42,
            time: 1_794_478_500,
            message: "index out of bounds\nsecond line",
            location: "src/battery.rs:10:5",
        };
        let text =
            report.format("   0: andromeda_system::battery::run\n   1: main", &["10:00:00 W [battery] low".to_owned()]);
        assert_eq!(
            text,
            "component: andromeda-system\nversion: 1.2.3\npid: 42\nsignal: panic\ntime: 2026-11-12T10:15:00Z\n\
             message: index out of bounds second line\nlocation: src/battery.rs:10:5\nbacktrace:\n\
             \x20  0: andromeda_system::battery::run\n   1: main\nlog:\n10:00:00 W [battery] low\n"
        );
        assert_eq!(iso_time(1_794_478_500, true), "20261112T101500Z");
    }

    #[test]
    fn ring_keeps_the_last_lines_oldest_first() {
        let mut ring = Ring { lines: Vec::new(), next: 0 };
        for i in 0..RING_LINES + 5 {
            ring.push(&format!("line {i}"));
        }
        let lines = ring.ordered();
        assert_eq!(lines.len(), RING_LINES);
        assert_eq!(lines[0], "line 5");
        assert_eq!(lines[RING_LINES - 1], format!("line {}", RING_LINES + 4));
    }

    /// Stands in for the panic hook: builds the report from inside a log argument's `Display`, then panics there.
    struct ReportsThenPanics(std::sync::mpsc::Sender<String>);

    impl std::fmt::Display for ReportsThenPanics {
        fn fmt(&self, _: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
            let _ = self.0.send(build_report("display-test", 0, "display failed", "src/x.rs:1:1"));
            panic!("display failed");
        }
    }

    #[test]
    fn panic_inside_a_log_argument_still_writes_the_report() {
        let (send, receive) = std::sync::mpsc::channel();
        let logger = std::thread::spawn(move || {
            let argument = ReportsThenPanics(send);
            let _ = std::panic::catch_unwind(|| record_line(log::Level::Error, "test", format_args!("{argument}")));
            record_line(log::Level::Info, "test", format_args!("after the panic"));
        });
        let report = receive.recv_timeout(std::time::Duration::from_secs(5)).unwrap_or_default();
        assert!(report.starts_with("component: display-test\n"), "no report within 5 s: {report}");
        assert!(report.contains("\nlog:\n"));
        assert!(logger.join().is_ok());
        assert!(ring_snapshot().unwrap_or_default().iter().any(|l| l.ends_with("[test] after the panic")));
    }
}
