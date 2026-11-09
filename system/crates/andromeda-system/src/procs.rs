//! Process samples for `per = "process"` rules: CPU wakeups per second and anonymous memory growth of every
//! user-space process. `/proc/<pid>/stat`, `status` and `task/*/schedstat` are world-readable, so root daemons count.

use std::collections::{HashMap, VecDeque};
use std::path::Path;
use std::time::{Duration, Instant};

use crate::expr::Value;
use crate::state::InstanceRow;

/// How far back the memory growth fit looks, and the least span it needs.
const GROWTH_WINDOW: Duration = Duration::from_secs(2 * 3600);
const GROWTH_MIN_SPAN: Duration = Duration::from_secs(3600);
const GROWTH_MIN_SAMPLES: usize = 6;
/// `PF_KTHREAD` in the flags field of `/proc/<pid>/stat`.
const PF_KTHREAD: u64 = 0x0020_0000;

struct Track {
    start: u64,
    wakeups: u64,
    at: Instant,
    /// (seconds since the first sample, MiB of anonymous memory).
    anon: VecDeque<(f64, f64)>,
    first: Instant,
}

#[derive(Default)]
pub struct Sampler {
    tracks: HashMap<u32, Track>,
    idle: bool,
}

/// `(comm, flags, starttime)` from `/proc/<pid>/stat`; comm may hold spaces and parentheses.
pub fn parse_stat(text: &str) -> Option<(String, u64, u64)> {
    let open = text.find('(')?;
    let close = text.rfind(')')?;
    let comm = text.get(open + 1..close)?.to_owned();
    // Fields after the comm start at field 3 (state); flags is field 9, starttime field 22.
    let rest: Vec<&str> = text.get(close + 1..)?.split_whitespace().collect();
    Some((comm, rest.get(6)?.parse().ok()?, rest.get(19)?.parse().ok()?))
}

/// `RssAnon` of `/proc/<pid>/status`, in kB.
pub fn rss_anon_kb(status: &str) -> Option<u64> {
    let line = status.lines().find(|l| l.starts_with("RssAnon:"))?;
    line.split_whitespace().nth(1)?.parse().ok()
}

/// `voluntary_ctxt_switches` of `/proc/<pid>/task/<tid>/status`: each one is the thread going to sleep, so the count is
/// its wakeups. Timeslices would also count a busy thread being preempted, which is CPU use, not waking the CPU.
pub fn voluntary_switches(status: &str) -> Option<u64> {
    let line = status.lines().find(|l| l.starts_with("voluntary_ctxt_switches:"))?;
    line.split_whitespace().nth(1)?.parse().ok()
}

/// Least-squares slope (per hour, from per-second x) and R² of `points`; R² is 0 when y does not vary.
pub fn fit(points: &[(f64, f64)]) -> Option<(f64, f64)> {
    if points.len() < 2 {
        return None;
    }
    #[allow(clippy::cast_precision_loss)] // a few hundred samples
    let n = points.len() as f64;
    let (mx, my) = points.iter().fold((0.0, 0.0), |(x, y), p| (x + p.0 / n, y + p.1 / n));
    let (mut sxy, mut sxx, mut syy) = (0.0, 0.0, 0.0);
    for (x, y) in points {
        sxy += (x - mx) * (y - my);
        sxx += (x - mx) * (x - mx);
        syy += (y - my) * (y - my);
    }
    if sxx == 0.0 {
        return None;
    }
    let r2 = if syy == 0.0 { 0.0 } else { sxy * sxy / (sxx * syy) };
    Some((sxy / sxx * 3600.0, r2))
}

fn read(path: &Path) -> Option<String> {
    std::fs::read_to_string(path).ok()
}

fn wakeups_of(task_dir: &Path) -> u64 {
    let Ok(tasks) = std::fs::read_dir(task_dir) else { return 0 };
    tasks
        .filter_map(Result::ok)
        .filter_map(|t| read(&t.path().join("status")))
        .filter_map(|s| voluntary_switches(&s))
        .sum()
}

impl Sampler {
    /// Reads every user-space process; each row is keyed `<comm> <pid>`. `idle` = the session was idle; a wake rate shows only when this sample and
    /// the one before it were both idle, so it never mixes in time the user worked.
    pub fn sample(&mut self, idle: bool, now: Instant) -> Vec<InstanceRow> {
        let both_idle = idle && self.idle;
        self.idle = idle;
        let me = std::process::id();
        let mut rows = Vec::new();
        let mut seen = Vec::new();
        let Ok(dir) = std::fs::read_dir("/proc") else { return rows };
        for entry in dir.filter_map(Result::ok) {
            let Some(pid) = entry.file_name().to_str().and_then(|n| n.parse::<u32>().ok()) else { continue };
            if pid == me {
                continue;
            }
            let base = entry.path();
            let Some((comm, flags, start)) = read(&base.join("stat")).and_then(|s| parse_stat(&s)) else { continue };
            if flags & PF_KTHREAD != 0 {
                continue;
            }
            let Some(kb) = read(&base.join("status")).and_then(|s| rss_anon_kb(&s)) else { continue };
            let wakeups = wakeups_of(&base.join("task"));
            #[allow(clippy::cast_precision_loss)] // kB as MiB
            let anon_mb = kb as f64 / 1024.0;
            seen.push(pid);
            let track = self.tracks.entry(pid).or_insert_with(|| Track {
                start,
                wakeups,
                at: now,
                anon: VecDeque::new(),
                first: now,
            });
            if track.start != start {
                *track = Track { start, wakeups, at: now, anon: VecDeque::new(), first: now };
            }
            let mut fields = vec![("proc.anon_mb", Value::Num(anon_mb))];
            let elapsed = now.duration_since(track.at).as_secs_f64();
            if both_idle && elapsed > 0.0 {
                #[allow(clippy::cast_precision_loss)] // a wakeup count
                let rate = wakeups.saturating_sub(track.wakeups) as f64 / elapsed;
                fields.push(("proc.wakeups_s", Value::Num(rate)));
            }
            track.wakeups = wakeups;
            track.at = now;
            let x = now.duration_since(track.first).as_secs_f64();
            track.anon.push_back((x, anon_mb));
            while track.anon.front().is_some_and(|(t, _)| x - t > GROWTH_WINDOW.as_secs_f64()) {
                track.anon.pop_front();
            }
            let span = track.anon.front().map_or(0.0, |(t, _)| x - t);
            if track.anon.len() >= GROWTH_MIN_SAMPLES && span >= GROWTH_MIN_SPAN.as_secs_f64() {
                let points: Vec<(f64, f64)> = track.anon.iter().copied().collect();
                if let Some((slope, r2)) = fit(&points) {
                    fields.push(("proc.growth_mb_h", Value::Num(slope)));
                    fields.push(("proc.growth_r2", Value::Num(r2)));
                }
            }
            rows.push((format!("{comm} {pid}"), fields));
        }
        self.tracks.retain(|pid, _| seen.contains(pid));
        rows
    }
}

#[cfg(test)]
#[allow(clippy::unwrap_used)]
mod tests {
    use super::*;

    #[test]
    fn stat_keeps_a_comm_with_spaces_and_parens() {
        let text = "812 (Web (Content) x) S 1 812 812 0 -1 4194560 10 0 0 0 5 3 0 0 20 0 4 0 12345 100 10";
        assert_eq!(parse_stat(text), Some(("Web (Content) x".to_owned(), 4_194_560, 12345)));
    }

    #[test]
    fn steady_growth_fits_its_slope_and_noise_does_not() {
        let steady: Vec<(f64, f64)> = (0..12).map(|i| (f64::from(i) * 600.0, 300.0 + f64::from(i) * 10.0)).collect();
        let (slope, r2) = fit(&steady).unwrap();
        assert!((slope - 60.0).abs() < 1e-9 && r2 > 0.999, "{slope} {r2}");
        let noisy: Vec<(f64, f64)> =
            (0..12).map(|i| (f64::from(i) * 600.0, if i % 2 == 0 { 300.0 } else { 500.0 })).collect();
        assert!(fit(&noisy).unwrap().1 < 0.1);
    }

    #[test]
    fn this_process_reads_as_user_space_with_wakeups() {
        let me = std::process::id();
        let (_, flags, _) = parse_stat(&read(Path::new(&format!("/proc/{me}/stat"))).unwrap()).unwrap();
        assert_eq!(flags & PF_KTHREAD, 0);
        assert!(wakeups_of(Path::new(&format!("/proc/{me}/task"))) > 0);
    }
}
