//! `/org/andromeda/System1/Diagnostics`: kernel stacks for a freeze report. A compositor or shell in developer
//! diagnostics mode asks when its main thread waits 250 ms or more in a kernel GPU path; the task that holds the lock
//! is one of the GPU tasks listed after the caller's own threads.

use std::collections::HashMap;
use std::fmt::Write as _;
use std::os::unix::fs::MetadataExt;
use std::path::Path;
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use zbus::message::Header;
use zbus::{Connection, interface};

use crate::{login, polkit};

pub const PATH: &str = "/org/andromeda/System1/Diagnostics";
const MAX_BYTES: usize = 256 * 1024;
const MIN_INTERVAL: Duration = Duration::from_secs(10);
const GPU_FRAMES: [&str; 7] = ["xe_", "i915_", "drm_", "ttm_", "dma_resv", "dma_fence", "ww_mutex"];

#[derive(Clone, Default)]
pub struct Diagnostics {
    last: Arc<Mutex<HashMap<u32, Instant>>>,
}

fn append_task(out: &mut String, task: &Path, stack: &str) {
    let comm = std::fs::read_to_string(task.join("comm")).unwrap_or_default();
    // The state is the first field after the parenthesised comm, which may itself hold spaces or ')'.
    let stat = std::fs::read_to_string(task.join("stat")).unwrap_or_default();
    let state = stat.rsplit_once(") ").and_then(|(_, rest)| rest.split(' ').next()).unwrap_or("?");
    let name = task.strip_prefix(task.ancestors().nth(3).unwrap_or(task)).unwrap_or(task);
    let _ = writeln!(out, "--- {} {} state={state}", name.display(), comm.trim_end());
    out.push_str(stack);
}

fn tasks(proc_root: &Path, pid: &str) -> Vec<std::path::PathBuf> {
    let Ok(dir) = std::fs::read_dir(proc_root.join(pid).join("task")) else { return Vec::new() };
    let mut tasks: Vec<_> = dir.flatten().map(|e| e.path()).collect();
    tasks.sort();
    tasks
}

/// Every thread of `pid`, then every other task whose kernel stack is in a GPU or DRM path; at most `MAX_BYTES`.
pub(crate) fn collect(proc_root: &Path, pid: u32) -> String {
    let own = pid.to_string();
    let mut out = String::new();
    for task in tasks(proc_root, &own) {
        append_task(&mut out, &task, &std::fs::read_to_string(task.join("stack")).unwrap_or_default());
    }
    let Ok(dir) = std::fs::read_dir(proc_root) else { return out };
    let mut pids: Vec<String> = dir
        .flatten()
        .filter_map(|e| e.file_name().into_string().ok())
        .filter(|name| name != &own && name.bytes().all(|b| b.is_ascii_digit()))
        .collect();
    pids.sort_by_key(|p| p.parse::<u32>().unwrap_or(0));
    for other in pids {
        for task in tasks(proc_root, &other) {
            if out.len() >= MAX_BYTES {
                out.truncate(MAX_BYTES);
                return out;
            }
            let Ok(stack) = std::fs::read_to_string(task.join("stack")) else { continue };
            if GPU_FRAMES.iter().any(|frame| stack.contains(frame)) {
                append_task(&mut out, &task, &stack);
            }
        }
    }
    out.truncate(out.len().min(MAX_BYTES));
    out
}

#[interface(name = "org.andromeda.System1.Diagnostics")]
impl Diagnostics {
    /// Kernel stacks of every thread of `pid`, then of every task waiting in a GPU or DRM path. `pid` must belong to
    /// the caller's user; one call per 10 s per user.
    async fn kernel_stacks(
        &self,
        pid: u32,
        #[zbus(header)] header: Header<'_>,
        #[zbus(connection)] conn: &Connection,
    ) -> zbus::fdo::Result<String> {
        let (uid, _) = login::caller(conn, &header).await?;
        polkit::check_with(conn, &header, "org.andromeda.system.diagnostics", false).await?;
        let owner =
            std::fs::metadata(format!("/proc/{pid}")).map_err(|e| zbus::fdo::Error::InvalidArgs(e.to_string()))?;
        if owner.uid() != uid {
            return Err(zbus::fdo::Error::AccessDenied(format!("process {pid} is not the caller's")));
        }
        {
            let mut last = self.last.lock().unwrap_or_else(std::sync::PoisonError::into_inner);
            let now = Instant::now();
            if last.get(&uid).is_some_and(|at| now.duration_since(*at) < MIN_INTERVAL) {
                return Err(zbus::fdo::Error::LimitsExceeded("one call per 10 s".into()));
            }
            last.insert(uid, now);
        }
        tokio::task::spawn_blocking(move || collect(Path::new("/proc"), pid))
            .await
            .map_err(|e| zbus::fdo::Error::Failed(e.to_string()))
    }
}

#[cfg(test)]
#[allow(clippy::unwrap_used)]
mod tests {
    use super::*;

    fn task(root: &Path, pid: &str, tid: &str, comm: &str, stack: &str) {
        let dir = root.join(pid).join("task").join(tid);
        std::fs::create_dir_all(&dir).unwrap();
        std::fs::write(dir.join("comm"), format!("{comm}\n")).unwrap();
        std::fs::write(dir.join("stat"), format!("{tid} ({comm}) D 1 2 3")).unwrap();
        std::fs::write(dir.join("stack"), stack).unwrap();
    }

    #[test]
    fn lists_own_threads_then_gpu_tasks_only() {
        let root = std::env::temp_dir().join(format!("diag-proc-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&root);
        task(&root, "50", "50", "andromeda", "[<0>] drm_exec_lock_obj+0x1/0x2\n");
        task(&root, "50", "51", "andromeda:wd", "[<0>] do_epoll_wait+0x1/0x2\n");
        task(&root, "7", "7", "kworker/u65:3", "[<0>] xe_bo_evict+0x1/0x2\n");
        task(&root, "9", "9", "bash", "[<0>] do_wait+0x1/0x2\n");
        let out = collect(&root, 50);
        let _ = std::fs::remove_dir_all(&root);
        let headers: Vec<_> = out.lines().filter(|l| l.starts_with("---")).collect();
        assert_eq!(
            headers,
            [
                "--- 50/task/50 andromeda state=D",
                "--- 50/task/51 andromeda:wd state=D",
                "--- 7/task/7 kworker/u65:3 state=D"
            ]
        );
        assert!(out.contains("xe_bo_evict"));
    }
}
