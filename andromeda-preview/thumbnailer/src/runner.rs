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
                let mut c = bwrap(&args, input, &work, Some(STATUS_FD));
                let mapped = writer.try_clone().ok().map(OwnedFd::from).map(|fd| {
                    c.fd_mappings(vec![FdMapping { parent_fd: fd, child_fd: STATUS_FD }]).is_ok()
                });
                if mapped != Some(true) {
                    return Outcome::Setup("status pipe".into());
                }
                c
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
                bwrap(&[OsString::from("/usr/bin/true")], &probe, &probe, None)
                    .stdin(Stdio::null())
                    .stdout(Stdio::null())
                    .stderr(Stdio::null())
                    .status()
                    .is_ok_and(|s| s.success())
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

/// The thumbnailer sees /usr, fonts, a private /tmp, its input read-only and its work directory; no network,
/// no other home files, no D-Bus or Wayland socket. The scope puts it in background.slice.
/// The child's fd for bwrap's JSON status.
const STATUS_FD: i32 = 3;

fn bwrap(args: &[OsString], input: &Path, work: &Path, status_fd: Option<i32>) -> Command {
    let mut c = Command::new("systemd-run");
    c.args(["--user", "--scope", "--quiet", "--collect", "--slice=background.slice"]);
    // A hostile file can make a decoder balloon or fork: cap memory (no swap) and tasks per thumbnail.
    c.args(["-p", "MemoryMax=512M", "-p", "MemorySwapMax=0", "-p", "TasksMax=32", "--"]);
    c.args(["bwrap", "--unshare-all", "--die-with-parent", "--new-session", "--clearenv"]);
    if let Some(fd) = status_fd {
        c.args(["--json-status-fd", &fd.to_string()]);
    }
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
    c
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
}
