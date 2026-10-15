//! The phone answering the desktop's browse requests: read-only listings, single entries, reads, and thumbnails under
//! the folders the app gives and its photo library. See "Browsing" in `link/ARCHITECTURE.md`.

use std::fs;
use std::io;
use std::os::unix::fs::FileExt;
use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::time::UNIX_EPOCH;

use link_proto::message::{
    FsData, FsEntries, FsEntry, FsError, FsList, FsRead, FsRefusal, FsStat, FsThumb, MAX_FS_CHUNK, MAX_FS_ENTRIES,
    MAX_FS_NAME, MAX_FS_READ, Message,
};

use crate::session::SessionHandle;

/// Browse requests one desktop may have in flight; more are answered `busy`.
pub const MAX_IN_FLIGHT: usize = 8;
/// The root the [`Library`] fills, one folder per album.
pub const PHOTOS: &str = "Photos";

/// The phone's photos and videos by album, as Android's media store has them. Called off the runtime; may block.
pub trait Library: Send + Sync {
    fn albums(&self) -> Vec<String>;
    fn album(&self, name: &str) -> Vec<LibraryItem>;
    /// A PNG of the image or video at `path`, at most `px` on its longer side; None when it has no picture.
    fn thumbnail(&self, path: &Path, px: u16) -> Option<Vec<u8>>;
}

pub struct LibraryItem {
    pub name: String,
    pub path: PathBuf,
    pub size: u64,
    /// Unix seconds.
    pub mtime: u64,
}

/// What the desktop sees at `/`: folders by name, and [`PHOTOS`] when there is a library.
#[derive(Clone, Default)]
pub struct Roots {
    folders: Vec<(String, PathBuf)>,
    library: Option<Arc<dyn Library>>,
}

/// What a browse path names.
enum Target<'a> {
    Top,
    Albums(&'a dyn Library),
    Album(&'a dyn Library, String),
    /// A file or folder on disk.
    Path(PathBuf),
}

impl Roots {
    pub fn new(folders: Vec<(String, PathBuf)>, library: Option<Arc<dyn Library>>) -> Self {
        Self { folders, library }
    }

    pub fn is_empty(&self) -> bool {
        self.folders.is_empty() && self.library.is_none()
    }

    /// What `path` names. A folder root's files must stay inside it once symlinks are resolved.
    fn resolve(&self, path: &str) -> Result<Target<'_>, FsRefusal> {
        let mut parts = path.strip_prefix('/').unwrap_or(path).splitn(2, '/');
        let root = parts.next().unwrap_or_default();
        if root.is_empty() {
            return Ok(Target::Top);
        }
        if let Some(library) = self.library.as_deref().filter(|_| root == PHOTOS) {
            let Some(rest) = parts.next() else { return Ok(Target::Albums(library)) };
            return match rest.split_once('/') {
                None => Ok(Target::Album(library, rest.to_owned())),
                Some((album, name)) => library
                    .album(album)
                    .into_iter()
                    .find(|item| item.name == name)
                    .map(|item| Target::Path(item.path))
                    .ok_or(FsRefusal::NotFound),
            };
        }
        let (_, base) = self.folders.iter().find(|(name, _)| name == root).ok_or(FsRefusal::NotFound)?;
        let base = base.canonicalize().map_err(|error| refusal(&error))?;
        let target = match parts.next() {
            Some(rest) => base.join(rest).canonicalize().map_err(|error| refusal(&error))?,
            None => base.clone(),
        };
        if target.starts_with(&base) { Ok(Target::Path(target)) } else { Err(FsRefusal::Denied) }
    }

    /// The entries of what `path` names, unsorted.
    fn list(&self, path: &str) -> Result<Vec<FsEntry>, FsRefusal> {
        let folder = |name: &str| FsEntry { name: name.to_owned(), dir: true, size: 0, mtime: 0 };
        match self.resolve(path)? {
            Target::Top => {
                let photos = self.library.as_ref().map(|_| folder(PHOTOS));
                Ok(self.folders.iter().map(|(name, _)| folder(name)).chain(photos).collect())
            }
            Target::Albums(library) => {
                Ok(library.albums().iter().filter(|name| named(name)).map(|name| folder(name)).collect())
            }
            Target::Album(library, name) => {
                Ok(library.album(&name).into_iter().filter(|item| named(&item.name)).map(entry).collect())
            }
            Target::Path(dir) => entries(&dir),
        }
    }

    fn stat(&self, path: &str) -> Result<FsEntry, FsRefusal> {
        let name = path.rsplit('/').next().filter(|name| !name.is_empty()).ok_or(FsRefusal::NotAFile)?;
        match self.resolve(path)? {
            Target::Top => Err(FsRefusal::NotAFile),
            Target::Albums(_) => Ok(FsEntry { name: name.to_owned(), dir: true, size: 0, mtime: 0 }),
            Target::Album(library, album) if library.albums().contains(&album) => {
                Ok(FsEntry { name: album, dir: true, size: 0, mtime: 0 })
            }
            Target::Album(..) => Err(FsRefusal::NotFound),
            Target::Path(path) => {
                let metadata = fs::metadata(path).map_err(|error| refusal(&error))?;
                Ok(FsEntry {
                    name: name.to_owned(),
                    dir: metadata.is_dir(),
                    size: if metadata.is_dir() { 0 } else { metadata.len() },
                    mtime: mtime(&metadata),
                })
            }
        }
    }

    fn file(&self, path: &str) -> Result<PathBuf, FsRefusal> {
        match self.resolve(path)? {
            Target::Path(path) if path.is_file() => Ok(path),
            _ => Err(FsRefusal::NotAFile),
        }
    }
}

/// Answers one browse request on `session`; the file work runs off the runtime.
pub async fn answer(roots: Roots, session: SessionHandle, request: Message) {
    let Some(req) = request_id(&request) else { return };
    let replies = tokio::task::spawn_blocking(move || match request {
        Message::FsList(list) => vec![listing(&roots, &list)],
        Message::FsStat(stat) => vec![stat_reply(&roots, &stat)],
        Message::FsRead(read) => reading(&roots, &read),
        Message::FsThumb(thumb) => thumbnail(&roots, &thumb),
        _ => Vec::new(),
    })
    .await
    .unwrap_or_else(|_| vec![error(req, FsRefusal::Io)]);
    for reply in replies {
        if let Err(error) = session.send(reply).await {
            return log::info!("answering a browse request: {error}");
        }
    }
}

/// The `req` of a browse request; None for any other message.
pub fn request_id(message: &Message) -> Option<u64> {
    match message {
        Message::FsList(FsList { req, .. })
        | Message::FsStat(FsStat { req, .. })
        | Message::FsRead(FsRead { req, .. })
        | Message::FsThumb(FsThumb { req, .. }) => Some(*req),
        _ => None,
    }
}

pub fn error(req: u64, reason: FsRefusal) -> Message {
    Message::FsError(FsError { req, reason })
}

fn listing(roots: &Roots, list: &FsList) -> Message {
    let mut entries = match roots.list(&list.path) {
        Ok(entries) => entries,
        Err(reason) => return error(list.req, reason),
    };
    entries.sort_by(|a, b| a.name.cmp(&b.name));
    let start = list.cursor.map_or(0, |cursor| cursor as usize).min(entries.len());
    let end = (start + MAX_FS_ENTRIES).min(entries.len());
    let next = (end < entries.len()).then(|| u32::try_from(end).unwrap_or(u32::MAX));
    Message::FsEntries(FsEntries { req: list.req, entries: entries.drain(start..end).collect(), next })
}

fn stat_reply(roots: &Roots, stat: &FsStat) -> Message {
    match roots.stat(&stat.path) {
        Ok(entry) => Message::FsEntries(FsEntries { req: stat.req, entries: vec![entry], next: None }),
        Err(reason) => error(stat.req, reason),
    }
}

/// A folder's entries; hidden ones and names the protocol cannot carry are left out.
fn entries(dir: &Path) -> Result<Vec<FsEntry>, FsRefusal> {
    if !dir.is_dir() {
        return Err(FsRefusal::NotAFolder);
    }
    let entries = fs::read_dir(dir).map_err(|error| refusal(&error))?.filter_map(Result::ok).filter_map(|entry| {
        let name = entry.file_name().into_string().ok().filter(|name| named(name))?;
        let metadata = entry.metadata().ok()?;
        Some(FsEntry {
            name,
            dir: metadata.is_dir(),
            size: if metadata.is_dir() { 0 } else { metadata.len() },
            mtime: mtime(&metadata),
        })
    });
    Ok(entries.collect())
}

fn entry(item: LibraryItem) -> FsEntry {
    FsEntry { name: item.name, dir: false, size: item.size, mtime: item.mtime }
}

fn named(name: &str) -> bool {
    !name.starts_with('.') && !name.contains('/') && (1..=MAX_FS_NAME).contains(&name.len())
}

fn mtime(metadata: &fs::Metadata) -> u64 {
    metadata.modified().ok().and_then(|time| time.duration_since(UNIX_EPOCH).ok()).map_or(0, |since| since.as_secs())
}

/// The `fs-data` frames answering `read`, the last one marked; a read at or past the end is one empty last frame.
fn reading(roots: &Roots, read: &FsRead) -> Vec<Message> {
    let file = roots.file(&read.path).and_then(|path| fs::File::open(path).map_err(|error| refusal(&error)));
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
    frames(read.req, read.offset, &buf)
}

fn thumbnail(roots: &Roots, thumb: &FsThumb) -> Vec<Message> {
    let picture = roots.file(&thumb.path).and_then(|path| {
        roots.library.as_ref().and_then(|library| library.thumbnail(&path, thumb.px)).ok_or(FsRefusal::NotFound)
    });
    match picture {
        Ok(png) if png.len() <= MAX_FS_READ as usize => frames(thumb.req, 0, &png),
        Ok(_) => vec![error(thumb.req, FsRefusal::Io)],
        Err(reason) => vec![error(thumb.req, reason)],
    }
}

fn frames(req: u64, mut offset: u64, bytes: &[u8]) -> Vec<Message> {
    let chunks: Vec<&[u8]> = if bytes.is_empty() { vec![&[]] } else { bytes.chunks(MAX_FS_CHUNK).collect() };
    let count = chunks.len();
    chunks
        .into_iter()
        .enumerate()
        .map(|(index, chunk)| {
            let data = FsData { req, offset, data: chunk.to_vec(), last: index + 1 == count };
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
