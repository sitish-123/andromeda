use std::ffi::OsStr;
use std::io::{self, ErrorKind};
use std::mem::MaybeUninit;
use std::os::unix::ffi::OsStrExt;
use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::time::Duration;

use rustix::fs::inotify;
use serde::Deserialize;
use serde_json::Value;
use tokio::io::unix::AsyncFd;
use tokio::io::{AsyncWriteExt, BufReader};
use tokio::net::UnixStream;
use tokio::sync::{mpsc, oneshot};

use crate::idl::{ActionEntry, SCHEMA_VERSION};
use crate::proto::{Line, read_line};
use crate::registry::{Hello, Seq, Shared};

fn is_provider_socket(name: &OsStr) -> bool {
    name.as_bytes().starts_with(b"andromeda-provider-") && Path::new(name).extension() == Some(OsStr::new("sock"))
}

/// Connects to every provider socket in `dir` now and to each one that appears later.
pub async fn watch(shared: Arc<Shared>, dir: PathBuf) -> io::Result<()> {
    let fd = inotify::init(inotify::CreateFlags::CLOEXEC | inotify::CreateFlags::NONBLOCK)?;
    inotify::add_watch(&fd, &dir, inotify::WatchFlags::CREATE | inotify::WatchFlags::MOVED_TO)?;
    // An OwnedFd keeps one open fd for its whole life, which is the condition of the unsafe AsyncFd::register.
    #[allow(deprecated)]
    let fd = AsyncFd::new(fd)?;
    scan(&shared, &dir)?;
    let mut buf = [MaybeUninit::<u8>::uninit(); 4096];
    loop {
        let mut ready = fd.readable().await?;
        let mut found = Vec::new();
        let mut overflow = false;
        let mut events = inotify::Reader::new(ready.get_inner(), &mut buf);
        loop {
            match events.next() {
                Ok(event) => {
                    overflow |= event.events().contains(inotify::ReadFlags::QUEUE_OVERFLOW);
                    if let Some(name) = event.file_name().map(|n| OsStr::from_bytes(n.to_bytes()))
                        && is_provider_socket(name)
                    {
                        found.push(dir.join(name));
                    }
                }
                Err(rustix::io::Errno::AGAIN) => break,
                Err(e) => return Err(e.into()),
            }
        }
        ready.clear_ready();
        if overflow {
            scan(&shared, &dir)?;
        }
        for path in found {
            tokio::spawn(link(Arc::clone(&shared), path));
        }
    }
}

fn scan(shared: &Arc<Shared>, dir: &Path) -> io::Result<()> {
    for entry in std::fs::read_dir(dir)? {
        let entry = entry?;
        if is_provider_socket(&entry.file_name()) {
            tokio::spawn(link(Arc::clone(shared), entry.path()));
        }
    }
    Ok(())
}

enum End {
    Closed,
    Resync,
}

async fn link(shared: Arc<Shared>, path: PathBuf) {
    loop {
        let stream = match connect(&path).await {
            Ok(stream) => stream,
            Err(e) => {
                log::debug!("{}: {e}", path.display());
                return;
            }
        };
        match session(&shared, stream, &path).await {
            End::Closed => return,
            End::Resync => log::info!("{}: event out of order, reading a new snapshot", path.display()),
        }
    }
}

/// A provider binds before it listens, so its inotify event can come before it accepts: retry a refused connect a
/// few times, with backoff, after each event.
async fn connect(path: &Path) -> io::Result<UnixStream> {
    let mut delay = Duration::from_millis(10);
    for _ in 0..5 {
        match UnixStream::connect(path).await {
            Err(e) if e.kind() == ErrorKind::ConnectionRefused => {
                tokio::time::sleep(delay).await;
                delay *= 2;
            }
            result => return result,
        }
    }
    UnixStream::connect(path).await
}

#[derive(Deserialize)]
struct HelloLine {
    op: String,
    provider: String,
    schema_version: u64,
    manifest_hash: String,
    actions: Vec<Value>,
}

async fn session(shared: &Arc<Shared>, stream: UnixStream, path: &Path) -> End {
    let (reader, mut writer) = stream.into_split();
    let mut reader = BufReader::new(reader);
    let mut buf = Vec::new();
    let hello = match read_line(&mut reader, &mut buf).await {
        Ok(Line::Complete) => serde_json::from_slice::<HelloLine>(&buf).ok(),
        _ => None,
    };
    let Some(hello) = hello.filter(|h| h.op == "hello" && h.schema_version == SCHEMA_VERSION) else {
        log::warn!("{}: no hello with schema_version {SCHEMA_VERSION}", path.display());
        return End::Closed;
    };
    let actions = match hello.actions.into_iter().map(ActionEntry::parse).collect::<Result<Vec<_>, _>>() {
        Ok(actions) => actions,
        Err(e) => {
            log::warn!("{}: hello from {} has a bad action entry: {e}", path.display(), hello.provider);
            return End::Closed;
        }
    };
    log::info!("{}: provider {} with {} actions", path.display(), hello.provider, actions.len());
    let (tx, mut rx) = mpsc::unbounded_channel::<String>();
    tokio::spawn(async move {
        while let Some(line) = rx.recv().await {
            if writer.write_all(line.as_bytes()).await.is_err() {
                break;
            }
        }
    });
    let (close, mut closed) = oneshot::channel();
    let hello = Hello { provider: hello.provider, manifest_hash: hello.manifest_hash, actions };
    let link = shared.register(hello, tx, close);
    loop {
        let read = tokio::select! {
            next = read_line(&mut reader, &mut buf) => next,
            _ = &mut closed => return End::Closed,
        };
        match read {
            Ok(Line::Complete) => {}
            Ok(Line::TooLong) => {
                log::warn!("{}: line over the size limit skipped", path.display());
                continue;
            }
            Ok(Line::Eof) | Err(_) => {
                shared.offline(&link);
                return End::Closed;
            }
        }
        let Ok(message) = serde_json::from_slice::<Value>(&buf) else {
            log::warn!("{}: line is not JSON", path.display());
            continue;
        };
        let seq = message.get("seq").and_then(Value::as_u64);
        match (message.get("op").and_then(Value::as_str), seq) {
            (Some("result"), _) => shared.complete(&link, message),
            (Some("snapshot"), Some(seq)) => match message.get("state") {
                Some(Value::Object(state)) => shared.snapshot(&link, seq, state.clone()),
                _ => log::warn!("{}: snapshot without a state object", path.display()),
            },
            (Some("event"), Some(seq)) => {
                if let Seq::Gap = shared.event(&link, seq, message.get("delta").unwrap_or(&Value::Null)) {
                    return End::Resync;
                }
            }
            _ => log::warn!("{}: unknown message", path.display()),
        }
    }
}
