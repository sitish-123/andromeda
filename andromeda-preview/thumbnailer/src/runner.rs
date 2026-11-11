//! Makes one thumbnail: cache hit, known failure, or a sandboxed thumbnailer run.

use crate::cache::{self, Flavor};
use crate::png;
use crate::registry::{Registry, Thumbnailer};
use std::fs;
use std::io::Write;
use std::os::unix::ffi::OsStringExt;
use command_fds::{CommandFdExt, FdMapping};
use std::ffi::{OsStr, OsString};
use std::os::fd::OwnedFd;
use std::os::unix::fs::{DirBuilderExt, MetadataExt, OpenOptionsExt};
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::time::{Duration, Instant};

#[derive(Debug, PartialEq, Eq)]
pub enum Error {
    /// Not a local `file://` URI, or the file is gone.
    NotLocal,
    /// No thumbnailer handles this type.
    Unsupported,
    /// A thumbnailer failed now or before, and the file has not changed since.
    Failed,
    Io(String),
}

/// How a thumbnailer is started.
#[derive(Clone, Debug)]
pub enum Sandbox {
    /// bwrap: no network, the input read-only, one private writable directory, in background.slice.
    Bwrap,
    /// Plain child process: tests with fake thumbnailers.
    None,
}

pub struct Runner {
    pub root: PathBuf,
    pub registry: Registry,
    pub sandbox: Sandbox,
    pub timeout: Duration,
}

fn local_path(uri: &str) -> Option<PathBuf> {
    let rest = uri.strip_prefix("file://")?;
    let mut out = Vec::with_capacity(rest.len());
    let bytes = rest.as_bytes();
    let mut i = 0;
    while i < bytes.len() {
        if bytes[i] == b'%' {
            let hex = std::str::from_utf8(bytes.get(i + 1..i + 3)?).ok()?;
            out.push(u8::from_str_radix(hex, 16).ok()?);
            i += 3;
        } else {
            out.push(bytes[i]);
            i += 1;
        }
    }
    let path = PathBuf::from(std::ffi::OsString::from_vec(out));
    path.is_absolute().then_some(path)
}

/// True when `png` is a thumbnail or marker made for this URI at this mtime.
fn current(png_path: &Path, uri: &str, mtime: i64) -> bool {
    fs::read(png_path).is_ok_and(|bytes| {
        png::text_value(&bytes, "Thumb::URI").as_deref() == Some(uri)
            && png::text_value(&bytes, "Thumb::MTime").is_some_and(|m| m == mtime.to_string())
    })
}

fn write_private(path: &Path, bytes: &[u8]) -> Result<(), Error> {
    let io = |e: std::io::Error| Error::Io(e.to_string());
    let dir = path.parent().ok_or_else(|| Error::Io("no parent".into()))?;
    fs::DirBuilder::new().recursive(true).mode(0o700).create(dir).map_err(io)?;
    let tmp = dir.join(format!(".{}.tmp", std::process::id()));
    let mut file = fs::OpenOptions::new().write(true).create(true).truncate(true).mode(0o600).open(&tmp).map_err(io)?;
    file.write_all(bytes).map_err(io)?;
    drop(file);
    fs::rename(&tmp, path).map_err(io)
}

impl Runner {
    /// The thumbnail path for `uri`, making it when needed. Blocks while a thumbnailer runs.
    ///
    /// # Errors
    /// See [`Error`]. A failed run writes the fail marker, so the next call returns `Failed` at once until the
    /// file's mtime changes.
    pub fn thumbnail(&self, uri: &str, mime: &str, flavor: Flavor) -> Result<PathBuf, Error> {
        let path = local_path(uri).ok_or(Error::NotLocal)?;
        let modified = fs::metadata(&path).map_err(|_| Error::NotLocal)?.mtime();
        let target = cache::thumbnail_path(&self.root, flavor, uri);
        if current(&target, uri, modified) {
            return Ok(target);
        }
        let fail = cache::fail_path(&self.root, uri);
        if current(&fail, uri, modified) {
            return Err(Error::Failed);
        }
        let thumbnailer = self.registry.for_mime(mime).ok_or(Error::Unsupported)?;
        match self.run(thumbnailer, uri, &path, flavor) {
            Outcome::Made(made) => {
                let mtime_text = modified.to_string();
                let fits = png::size(&made).is_some_and(|(w, h)| w > 0 && h > 0 && w <= flavor.size() && h <= flavor.size());
                let tagged = png::with_text(&made, &[("Thumb::URI", uri), ("Thumb::MTime", &mtime_text)]);
                match tagged.filter(|_| fits) {
                    Some(tagged) => {
                        write_private(&target, &tagged)?;
                        Ok(target)
                    }
                    None => self.mark_failed(uri, modified),
                }
            }
            Outcome::ThumbnailerFailed => self.mark_failed(uri, modified),
            // Not the file's fault: no marker, so a later request tries again.
            Outcome::Setup(why) => Err(Error::Io(why)),
        }
    }

    fn mark_failed(&self, uri: &str, mtime: i64) -> Result<PathBuf, Error> {
        let marker = png::marker(&[("Thumb::URI", uri), ("Thumb::MTime", &mtime.to_string())]);
        write_private(&cache::fail_path(&self.root, uri), &marker)?;
        Err(Error::Failed)
    }

    /// Runs the thumbnailer into a private directory.
    fn run(&self, thumbnailer: &Thumbnailer, uri: &str, input: &Path, flavor: Flavor) -> Outcome {
        let work = self.root.join(format!(".work-{}-{}", std::process::id(), rand_suffix()));
        if let Err(e) = fs::DirBuilder::new().recursive(true).mode(0o700).create(&work) {
            return Outcome::Setup(e.to_string());
        }
        let output = work.join("out.png");
        let size = flavor.size().to_string();
        let args: Vec<OsString> =
            thumbnailer.exec.iter().map(|a| expand(a, input.as_os_str(), uri, output.as_os_str(), &size)).collect();
        // bwrap writes {"exit-code": N} to this pipe only when the thumbnailer ran; without it a non-zero
        // exit is a sandbox setup error, not the file's fault.
        let mut status_pipe = match self.sandbox {
            Sandbox::Bwrap => match std::io::pipe() {
                Ok(pipe) => Some(pipe),
                Err(e) => return Outcome::Setup(e.to_string()),
            },
            Sandbox::None => None,
        };
        let mut command = match &status_pipe {
            None => {
                let mut c = Command::new(&args[0]);
                c.args(&args[1..]);
                c
            }
            Some((_, writer)) => {
                let status = match writer.try_clone() {
                    Ok(fd) => OwnedFd::from(fd),
                    Err(e) => return Outcome::Setup(e.to_string()),
                };
                match bwrap(&args, input, &work, Some(status)) {
                    Ok(c) => c,
                    Err(why) => return Outcome::Setup(why),
                }
            }
        };
        command.stdin(Stdio::null()).stdout(Stdio::null()).stderr(Stdio::null());
        let spawned = command.spawn();
        drop(command);
        let started = |pipe: Option<(std::io::PipeReader, std::io::PipeWriter)>| {
            pipe.is_none_or(|(mut reader, writer)| {
                use std::io::Read;
                drop(writer);
                let mut text = String::new();
                let _ = reader.read_to_string(&mut text);
                text.contains("\"exit-code\"")
            })
        };
        let outcome = match spawned {
            Err(e) => Outcome::Setup(e.to_string()),
            Ok(mut child) => {
                let deadline = Instant::now() + self.timeout;
                loop {
                    match child.try_wait() {
                        Ok(Some(status)) if status.success() => {
                            break read_output(&output).map_or(Outcome::ThumbnailerFailed, Outcome::Made);
                        }
                        Ok(Some(_)) if started(status_pipe.take()) => break Outcome::ThumbnailerFailed,
                        Ok(Some(_)) => break Outcome::Setup("sandbox did not start".into()),
                        Ok(None) if Instant::now() < deadline => std::thread::sleep(Duration::from_millis(20)),
                        _ => {
                            let _ = child.kill();
                            let _ = child.wait();
                            // A file that hangs its thumbnailer would stall the one-at-a-time queue on every
                            // request: mark it like any other failure.
                            log::warn!("{} timed out on {uri}", thumbnailer.name);
                            break Outcome::ThumbnailerFailed;
                        }
                    }
                }
            }
        };
        let _ = fs::remove_dir_all(&work);
        outcome
    }

    /// Checks once that the sandbox can start at all, so a missing bwrap or user manager is not taken for a
    /// broken file.
    #[must_use]
    pub fn sandbox_works(&self) -> bool {
        match self.sandbox {
            Sandbox::None => true,
            Sandbox::Bwrap => {
                let probe = std::env::temp_dir();
                bwrap(&[OsString::from("/usr/bin/true")], &probe, &probe, None).is_ok_and(|mut c| {
                    c.stdin(Stdio::null()).stdout(Stdio::null()).stderr(Stdio::null()).status().is_ok_and(|s| s.success())
                })
            }
        }
    }
}

enum Outcome {
    Made(Vec<u8>),
    /// The thumbnailer ran and failed, or left no usable PNG: the file's fault, so it gets a marker.
    ThumbnailerFailed,
    Setup(String),
}

/// Largest thumbnail read back; an xx-large RGBA PNG stays far below it.
const MAX_OUTPUT: u64 = 32 << 20;

/// Reads the thumbnailer's output without following a link it planted and without blocking on a FIFO.
fn read_output(path: &Path) -> Option<Vec<u8>> {
    use std::io::Read;
    let file = fs::OpenOptions::new()
        .read(true)
        .custom_flags(libc::O_NOFOLLOW | libc::O_NONBLOCK)
        .open(path)
        .ok()?;
    if !file.metadata().ok()?.is_file() {
        return None;
    }
    let mut bytes = Vec::new();
    file.take(MAX_OUTPUT + 1).read_to_end(&mut bytes).ok()?;
    (bytes.len() as u64 <= MAX_OUTPUT).then_some(bytes)
}

/// Expands %i, %u, %o, %s and %% in one pass, so text that came from a file name is never expanded again.
fn expand(token: &str, input: &OsStr, uri: &str, output: &OsStr, size: &str) -> OsString {
    let mut out = OsString::new();
    let mut chars = token.chars();
    while let Some(c) = chars.next() {
        if c != '%' {
            out.push(c.encode_utf8(&mut [0; 4]));
            continue;
        }
        match chars.next() {
            Some('i') => out.push(input),
            Some('u') => out.push(uri),
            Some('o') => out.push(output),
            Some('s') => out.push(size),
            Some('%') => out.push("%"),
            Some(other) => {
                out.push("%");
                out.push(other.encode_utf8(&mut [0; 4]));
            }
            None => out.push("%"),
        }
    }
    out
}

fn rand_suffix() -> u64 {
    use std::hash::{BuildHasher, RandomState};
    RandomState::new().hash_one(Instant::now())
}

/// The child's fds for bwrap's JSON status and its seccomp program.
const STATUS_FD: i32 = 3;
const SECCOMP_FD: i32 = 4;

/// Syscalls a decoder never needs that widen the kernel surface: debugging other processes, kernel modules and
/// images, eBPF, perf, userfaultfd, keyrings, mounts and namespaces, io_uring, clock and system state.
const DENIED: &[libc::c_long] = &[
    libc::SYS_ptrace,
    libc::SYS_process_vm_readv,
    libc::SYS_process_vm_writev,
    libc::SYS_kexec_load,
    libc::SYS_kexec_file_load,
    libc::SYS_init_module,
    libc::SYS_finit_module,
    libc::SYS_delete_module,
    libc::SYS_bpf,
    libc::SYS_perf_event_open,
    libc::SYS_userfaultfd,
    libc::SYS_keyctl,
    libc::SYS_add_key,
    libc::SYS_request_key,
    libc::SYS_mount,
    libc::SYS_umount2,
    libc::SYS_pivot_root,
    libc::SYS_unshare,
    libc::SYS_setns,
    libc::SYS_open_tree,
    libc::SYS_move_mount,
    libc::SYS_fsopen,
    libc::SYS_fsconfig,
    libc::SYS_fsmount,
    libc::SYS_fspick,
    libc::SYS_mount_setattr,
    libc::SYS_io_uring_setup,
    libc::SYS_io_uring_enter,
    libc::SYS_io_uring_register,
    libc::SYS_personality,
    libc::SYS_acct,
    libc::SYS_swapon,
    libc::SYS_swapoff,
    libc::SYS_reboot,
    libc::SYS_clock_settime,
    libc::SYS_clock_adjtime,
    libc::SYS_settimeofday,
    libc::SYS_adjtimex,
    libc::SYS_syslog,
    libc::SYS_quotactl,
    libc::SYS_open_by_handle_at,
    libc::SYS_name_to_handle_at,
    #[cfg(target_arch = "x86_64")]
    libc::SYS_ioperm,
    #[cfg(target_arch = "x86_64")]
    libc::SYS_iopl,
];

const NAMESPACE_FLAGS: u32 = (libc::CLONE_NEWNS
    | libc::CLONE_NEWCGROUP
    | libc::CLONE_NEWUTS
    | libc::CLONE_NEWIPC
    | libc::CLONE_NEWUSER
    | libc::CLONE_NEWPID
    | libc::CLONE_NEWNET
    | libc::CLONE_NEWTIME) as u32;

/// The seccomp program for bwrap's `--seccomp`: raw `struct sock_filter` entries in native byte order.
/// Denied syscalls and clone with a namespace flag return EPERM; clone3 returns ENOSYS, so libc falls back to
/// clone, whose flags a filter can read. A socket of any family but `AF_UNIX` returns EAFNOSUPPORT: decoders use
/// only local sockets, and each other family is kernel surface. Another architecture, or an x32 call, kills the
/// process.
/// None on an architecture this filter does not know.
fn seccomp_program() -> Option<Vec<u8>> {
    const LD_W_ABS: u16 = 0x20;
    const JEQ_K: u16 = 0x15;
    const JGE_K: u16 = 0x35;
    const JSET_K: u16 = 0x45;
    const RET_K: u16 = 0x06;
    const ALLOW: u32 = 0x7fff_0000;
    const ERRNO: u32 = 0x0005_0000;
    const KILL_PROCESS: u32 = 0x8000_0000;
    const OFFSET_NR: u32 = 0;
    const OFFSET_ARCH: u32 = 4;
    // Low 32 bits of args[0].
    const OFFSET_ARG0: u32 = if cfg!(target_endian = "little") { 16 } else { 20 };
    const X32_BIT: u32 = 0x4000_0000;
    let arch: u32 = match std::env::consts::ARCH {
        "x86_64" => 62 | 0x8000_0000 | 0x4000_0000,
        "aarch64" => 183 | 0x8000_0000 | 0x4000_0000,
        _ => return None,
    };
    let nr = |n: libc::c_long| u32::try_from(n).ok();
    let (clone, clone3, socket) = (nr(libc::SYS_clone)?, nr(libc::SYS_clone3)?, nr(libc::SYS_socket)?);
    let denied: Vec<u32> = DENIED.iter().map(|&n| nr(n)).collect::<Option<_>>()?;

    // Head: arch check, load nr, x32 check. Then one jump per denied syscall, clone3, clone and socket. Tail at
    // the end.
    let head = 4;
    let checks = denied.len() + 3;
    let tail = head + checks;
    let (clone_args, socket_args, eperm, enosys, kill) = (tail + 1, tail + 4, tail + 8, tail + 9, tail + 10);
    let mut prog: Vec<(u16, u8, u8, u32)> = Vec::with_capacity(tail + 11);
    // Jump offsets count from the next instruction and only go forward.
    let to = |from: usize, target: usize| u8::try_from(target - from - 1).ok();
    prog.push((LD_W_ABS, 0, 0, OFFSET_ARCH));
    prog.push((JEQ_K, 0, to(1, kill)?, arch));
    prog.push((LD_W_ABS, 0, 0, OFFSET_NR));
    let x32 = if cfg!(target_arch = "x86_64") { X32_BIT } else { u32::MAX };
    prog.push((JGE_K, to(3, kill)?, 0, x32));
    for &n in &denied {
        let at = prog.len();
        prog.push((JEQ_K, to(at, eperm)?, 0, n));
    }
    let at = prog.len();
    prog.push((JEQ_K, to(at, enosys)?, 0, clone3));
    let at = prog.len();
    prog.push((JEQ_K, to(at, clone_args)?, 0, clone));
    let at = prog.len();
    prog.push((JEQ_K, to(at, socket_args)?, 0, socket));
    prog.push((RET_K, 0, 0, ALLOW));
    prog.push((LD_W_ABS, 0, 0, OFFSET_ARG0));
    prog.push((JSET_K, to(clone_args + 1, eperm)?, 0, NAMESPACE_FLAGS));
    prog.push((RET_K, 0, 0, ALLOW));
    prog.push((LD_W_ABS, 0, 0, OFFSET_ARG0));
    prog.push((JEQ_K, 0, 1, libc::AF_UNIX as u32));
    prog.push((RET_K, 0, 0, ALLOW));
    prog.push((RET_K, 0, 0, ERRNO | libc::EAFNOSUPPORT as u32));
    prog.push((RET_K, 0, 0, ERRNO | libc::EPERM as u32));
    prog.push((RET_K, 0, 0, ERRNO | libc::ENOSYS as u32));
    prog.push((RET_K, 0, 0, KILL_PROCESS));
    debug_assert_eq!(prog.len(), kill + 1);
    let mut bytes = Vec::with_capacity(prog.len() * 8);
    for (code, jt, jf, k) in prog {
        bytes.extend_from_slice(&code.to_ne_bytes());
        bytes.extend_from_slice(&[jt, jf]);
        bytes.extend_from_slice(&k.to_ne_bytes());
    }
    Some(bytes)
}

/// A pipe that holds the seccomp program; the program stays far below the pipe buffer, so the write never blocks.
fn seccomp_fd() -> Result<OwnedFd, String> {
    let program = seccomp_program().ok_or("no seccomp filter for this architecture")?;
    let (reader, mut writer) = std::io::pipe().map_err(|e| e.to_string())?;
    writer.write_all(&program).map_err(|e| e.to_string())?;
    Ok(OwnedFd::from(reader))
}

/// The thumbnailer sees /usr, fonts, a private /tmp, its input read-only and its work directory; no network,
/// no other home files, no D-Bus or Wayland socket, and a seccomp filter. The scope puts it in background.slice.
///
/// # Errors
/// The seccomp program or the fd mapping cannot be set up; the thumbnailer must then not run.
fn bwrap(args: &[OsString], input: &Path, work: &Path, status: Option<OwnedFd>) -> Result<Command, String> {
    let mut fds = vec![FdMapping { parent_fd: seccomp_fd()?, child_fd: SECCOMP_FD }];
    let status_fd = status.map(|fd| {
        fds.push(FdMapping { parent_fd: fd, child_fd: STATUS_FD });
        STATUS_FD
    });
    let mut c = Command::new("systemd-run");
    c.fd_mappings(fds).map_err(|_| "fd mapping".to_string())?;
    c.args(["--user", "--scope", "--quiet", "--collect", "--slice=background.slice"]);
    // A hostile file can make a decoder balloon or fork: cap memory (no swap) and tasks per thumbnail.
    c.args(["-p", "MemoryMax=512M", "-p", "MemorySwapMax=0", "-p", "TasksMax=32", "--"]);
    c.args(["bwrap", "--unshare-all", "--die-with-parent", "--new-session", "--clearenv"]);
    if let Some(fd) = status_fd {
        c.args(["--json-status-fd", &fd.to_string()]);
    }
    c.args(["--seccomp", &SECCOMP_FD.to_string()]);
    c.args(["--setenv", "PATH", "/usr/bin", "--setenv", "HOME", "/tmp"]);
    c.args(["--ro-bind", "/usr", "/usr", "--symlink", "usr/bin", "/bin", "--symlink", "usr/lib", "/lib"]);
    c.args(["--symlink", "usr/lib", "/lib64", "--symlink", "usr/bin", "/sbin"]);
    for dir in ["/etc/fonts", "/etc/ld.so.cache", "/var/cache/fontconfig"] {
        c.args(["--ro-bind-try", dir, dir]);
    }
    c.args(["--proc", "/proc", "--dev", "/dev", "--tmpfs", "/tmp"]);
    c.arg("--ro-bind").arg(input).arg(input);
    c.arg("--bind").arg(work).arg(work);
    c.arg("--").args(args);
    Ok(c)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn runner(dir: &Path) -> Runner {
        Runner { root: dir.join("cache"), registry: Registry::default(), sandbox: Sandbox::Bwrap, timeout: Duration::from_secs(20) }
    }

    fn program(path: &str) -> Thumbnailer {
        Thumbnailer { name: "probe".into(), exec: vec![path.into()] }
    }

    /// A bwrap that cannot set up (its bind source is missing) is a setup error; a thumbnailer that started and
    /// failed is the file's failure.
    #[test]
    fn sandbox_setup_errors_are_told_apart_from_thumbnailer_failures() {
        let dir = tempfile::tempdir().unwrap();
        let r = runner(dir.path());
        if !r.sandbox_works() {
            eprintln!("skipped: no working sandbox");
            return;
        }
        let input = dir.path().join("input");
        std::fs::write(&input, b"x").unwrap();
        let missing = dir.path().join("missing");
        assert!(matches!(r.run(&program("/usr/bin/true"), "file:///x", &missing, Flavor::Normal), Outcome::Setup(_)));
        assert!(matches!(r.run(&program("/usr/bin/false"), "file:///x", &input, Flavor::Normal), Outcome::ThumbnailerFailed));
    }

    /// A decoder inside the sandbox cannot make a namespace (EPERM) or a socket other than `AF_UNIX`
    /// (EAFNOSUPPORT). Each probe copies the input to the output only when the call failed with that error.
    #[test]
    fn sandbox_denies_namespaces_and_sockets() {
        let dir = tempfile::tempdir().unwrap();
        let r = runner(dir.path());
        if !r.sandbox_works() {
            eprintln!("skipped: no working sandbox");
            return;
        }
        let input = dir.path().join("input.png");
        std::fs::write(&input, png::marker(&[])).unwrap();
        let namespace = "unshare -U /usr/bin/true 2>&1 | grep -q 'Operation not permitted' && cp \"$1\" \"$2\"";
        let socket = "import errno, shutil, socket, sys\n\
            try:\n    socket.socket(socket.AF_INET, socket.SOCK_DGRAM)\n\
            except OSError as e:\n    e.errno == errno.EAFNOSUPPORT and shutil.copy(sys.argv[1], sys.argv[2])";
        for exec in [["/usr/bin/sh", "-c", namespace, "sh", "%i", "%o"], ["/usr/bin/python3", "-I", "-c", socket, "%i", "%o"]] {
            let probe = Thumbnailer { name: "probe".into(), exec: exec.map(String::from).to_vec() };
            assert!(matches!(r.run(&probe, "file:///x", &input, Flavor::Normal), Outcome::Made(_)), "{exec:?}");
        }
    }
}
