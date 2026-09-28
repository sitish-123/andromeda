//! The phone answering the desktop's browse requests: read-only listings and reads under the roots the app gives.
//! See "Browsing" in `link/ARCHITECTURE.md`.

use std::fs;
use std::io;
use std::os::unix::fs::FileExt;
use std::path::{Path, PathBuf};
use std::time::UNIX_EPOCH;

use link_proto::message::{
    FsData, FsEntries, FsEntry, FsError, FsList, FsRead, FsRefusal, MAX_FS_CHUNK, MAX_FS_ENTRIES, MAX_FS_NAME, Message,
};

use crate::session::SessionHandle;

/// Browse requests one desktop may have in flight; more are answered `busy`.
pub const MAX_IN_FLIGHT: usize = 8;

/// The folders the desktop sees at `/`, by name.
#[derive(Debug, Clone, Default)]
pub struct Roots(Vec<(String, PathBuf)>);

impl Roots {
    pub fn new(roots: Vec<(String, PathBuf)>) -> Self {
        Self(roots)
    }

    pub fn is_empty(&self) -> bool {
        self.0.is_empty()
    }

    /// The file `path` names, which must stay inside its root once symlinks are resolved.
    fn resolve(&self, path: &str) -> Result<PathBuf, FsRefusal> {
        let mut parts = path.strip_prefix('/').unwrap_or(path).splitn(2, '/');
        let root = parts.next().unwrap_or_default();
        let (_, base) = self.0.iter().find(|(name, _)| name == root).ok_or(FsRefusal::NotFound)?;
        let base = base.canonicalize().map_err(|error| refusal(&error))?;
        let target = match parts.next() {
            Some(rest) => base.join(rest).canonicalize().map_err(|error| refusal(&error))?,
            None => base.clone(),
        };
        if target.starts_with(&base) { Ok(target) } else { Err(FsRefusal::Denied) }
    }
}

/// Answers one `fs-list` or `fs-read` on `session`; the file work runs off the runtime.
pub async fn answer(roots: Roots, session: SessionHandle, request: Message) {
    let replies = match request {
        Message::FsList(list) => {
            let req = list.req;
            tokio::task::spawn_blocking(move || vec![listing(&roots, &list)])
                .await
                .unwrap_or_else(|_| vec![error(req, FsRefusal::Io)])
        }
        Message::FsRead(read) => {
            let req = read.req;
            tokio::task::spawn_blocking(move || reading(&roots, &read))
                .await
                .unwrap_or_else(|_| vec![error(req, FsRefusal::Io)])
        }
        _ => return,
    };
    for reply in replies {
        if let Err(error) = session.send(reply).await {
            return log::info!("answering a browse request: {error}");
        }
    }
}

pub fn error(req: u64, reason: FsRefusal) -> Message {
    Message::FsError(FsError { req, reason })
}

fn listing(roots: &Roots, list: &FsList) -> Message {
    let entries = if list.path == "/" {
        Ok(roots.0.iter().map(|(name, _)| FsEntry { name: name.clone(), dir: true, size: 0, mtime: 0 }).collect())
    } else {
        roots.resolve(&list.path).and_then(|dir| entries(&dir))
    };
    let mut entries: Vec<FsEntry> = match entries {
        Ok(entries) => entries,
        Err(reason) => return error(list.req, reason),
    };
    entries.sort_by(|a, b| a.name.cmp(&b.name));
    let start = list.cursor.map_or(0, |cursor| cursor as usize).min(entries.len());
    let end = (start + MAX_FS_ENTRIES).min(entries.len());
    let next = (end < entries.len()).then(|| u32::try_from(end).unwrap_or(u32::MAX));
    Message::FsEntries(FsEntries { req: list.req, entries: entries.drain(start..end).collect(), next })
}

/// A folder's entries; hidden ones and names the protocol cannot carry are left out.
fn entries(dir: &Path) -> Result<Vec<FsEntry>, FsRefusal> {
    if !dir.is_dir() {
        return Err(FsRefusal::NotAFolder);
    }
    let entries = fs::read_dir(dir).map_err(|error| refusal(&error))?.filter_map(Result::ok).filter_map(|entry| {
        let name = entry.file_name().into_string().ok()?;
        if name.starts_with('.') || name.len() > MAX_FS_NAME {
            return None;
        }
        let metadata = entry.metadata().ok()?;
        let mtime = metadata.modified().ok()?.duration_since(UNIX_EPOCH).map_or(0, |since| since.as_secs());
        Some(FsEntry { name, dir: metadata.is_dir(), size: if metadata.is_dir() { 0 } else { metadata.len() }, mtime })
    });
    Ok(entries.collect())
}

/// The `fs-data` frames answering `read`, the last one marked; a read at or past the end is one empty last frame.
fn reading(roots: &Roots, read: &FsRead) -> Vec<Message> {
    let file = roots.resolve(&read.path).and_then(|path| {
        if !path.is_file() {
            return Err(FsRefusal::NotAFile);
        }
        fs::File::open(path).map_err(|error| refusal(&error))
    });
    let file = match file {
        Ok(file) => file,
        Err(reason) => return vec![error(read.req, reason)],
    };
    let mut buf = vec![0; read.len as usize];
    let mut filled = 0;
    while filled < buf.len() {
        match file.read_at(&mut buf[filled..], read.offset + filled as u64) {
            Ok(0) => break,
            Ok(n) => filled += n,
            Err(error) if error.kind() == io::ErrorKind::Interrupted => {}
            Err(error) => return vec![self::error(read.req, refusal(&error))],
        }
    }
    buf.truncate(filled);
    let chunks: Vec<&[u8]> = if buf.is_empty() { vec![&[]] } else { buf.chunks(MAX_FS_CHUNK).collect() };
    let count = chunks.len();
    let mut offset = read.offset;
    chunks
        .into_iter()
        .enumerate()
        .map(|(index, chunk)| {
            let data = FsData { req: read.req, offset, data: chunk.to_vec(), last: index + 1 == count };
            offset += chunk.len() as u64;
            Message::FsData(data)
        })
        .collect()
}

fn refusal(error: &io::Error) -> FsRefusal {
    match error.kind() {
        io::ErrorKind::NotFound => FsRefusal::NotFound,
        io::ErrorKind::PermissionDenied => FsRefusal::Denied,
        _ => FsRefusal::Io,
    }
}
