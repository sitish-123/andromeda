//! Where received files go: part files in the download directory, their state records, and publishing a verified
//! file under a name that never overwrites. See `link/ARCHITECTURE.md` (Files).

use std::fs::{self, File, OpenOptions};
use std::io::{self, Write};
use std::os::unix::fs::{FileExt, OpenOptionsExt};
use std::path::{Path, PathBuf};
use std::time::{SystemTime, UNIX_EPOCH};

use link_proto::message::TransferId;
use ring::digest;
use serde::{Deserialize, Serialize};

use crate::Error;
use crate::identity::DeviceId;

/// `NAME_MAX` on Linux filesystems.
const MAX_NAME: usize = 255;
const PART_SUFFIX: &str = ".linkpart";
/// Room for a leading dot, ` (NNN)`, and [`PART_SUFFIX`] inside [`MAX_NAME`].
const MAX_PART_STEM: usize = MAX_NAME - 1 - 6 - PART_SUFFIX.len();
const REHASH_CHUNK: usize = 1 << 20;

pub struct Inbox {
    downloads: PathBuf,
    records: PathBuf,
}

/// Free and total bytes of the download directory's filesystem.
pub struct Space {
    pub free: u64,
    pub total: u64,
}

/// A receiver's state for one transfer, kept on disk until it finishes or expires.
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Record {
    pub transfer: String,
    pub peer: DeviceId,
    /// Unix seconds.
    pub created: u64,
    pub files: Vec<RecordFile>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct RecordFile {
    pub id: u64,
    /// Already sanitized.
    pub name: String,
    pub size: u64,
    pub sha256: Vec<u8>,
    pub part: PathBuf,
    /// Bytes known to be on disk.
    pub durable: u64,
    #[serde(default)]
    pub done: Option<bool>,
    #[serde(default)]
    pub path: Option<PathBuf>,
}

/// An open part file and the hash of everything written to it.
pub struct Part {
    file: File,
    hasher: digest::Context,
    offset: u64,
}

/// The basename only; no NUL, control characters, or leading dots; at most 255 bytes cut on a UTF-8 boundary; `file`
/// when nothing is left.
pub fn sanitize(name: &str) -> String {
    let base = name.rsplit(['/', '\\']).next().unwrap_or_default();
    let cleaned: String = base.chars().filter(|c| !c.is_control()).collect();
    let name = cut(cleaned.trim_start_matches('.'), MAX_NAME);
    if name.is_empty() { "file".to_owned() } else { name.to_owned() }
}

fn cut(text: &str, max: usize) -> &str {
    let mut end = text.len().min(max);
    while !text.is_char_boundary(end) {
        end -= 1;
    }
    &text[..end]
}

/// `name`, or `stem (n).ext` for `n` > 0, within [`MAX_NAME`] bytes.
fn numbered(name: &str, n: u32) -> String {
    if n == 0 {
        return name.to_owned();
    }
    let (stem, ext) = match name.rfind('.') {
        Some(dot) if dot > 0 => name.split_at(dot),
        _ => (name, ""),
    };
    let suffix = format!(" ({n}){ext}");
    let stem = cut(stem, MAX_NAME.saturating_sub(suffix.len()));
    format!("{stem}{suffix}")
}

pub fn now() -> u64 {
    SystemTime::now().duration_since(UNIX_EPOCH).map_or(0, |elapsed| elapsed.as_secs())
}

impl Inbox {
    pub fn new(downloads: PathBuf, state: &Path) -> Result<Self, Error> {
        let records = state.join("transfers");
        fs::create_dir_all(&records)?;
        Ok(Self { downloads, records })
    }

    pub fn space(&self) -> Result<Space, Error> {
        fs::create_dir_all(&self.downloads)?;
        let stat = rustix::fs::statvfs(&self.downloads).map_err(io::Error::from)?;
        Ok(Space {
            free: stat.f_bavail.saturating_mul(stat.f_frsize),
            total: stat.f_blocks.saturating_mul(stat.f_frsize),
        })
    }

    /// Creates `.<name>.linkpart` with `O_EXCL`, numbering it when taken.
    pub fn create_part(&self, name: &str) -> Result<PathBuf, Error> {
        fs::create_dir_all(&self.downloads)?;
        let stem = cut(name, MAX_PART_STEM);
        for n in 0..1000 {
            let path = self.downloads.join(format!(".{}{PART_SUFFIX}", numbered(stem, n)));
            match OpenOptions::new().write(true).create_new(true).mode(0o600).open(&path) {
                Ok(_) => return Ok(path),
                Err(error) if error.kind() == io::ErrorKind::AlreadyExists => {}
                Err(error) => return Err(error.into()),
            }
        }
        Err(io::Error::new(io::ErrorKind::AlreadyExists, "no free part name").into())
    }

    /// Hard-links the verified part to the first free final name, then unlinks the part.
    pub fn publish(&self, part: &Path, name: &str) -> Result<PathBuf, Error> {
        for n in 0..10_000 {
            let path = self.downloads.join(numbered(name, n));
            match place(part, &path) {
                Ok(()) => return Ok(path),
                Err(error) if error.kind() == io::ErrorKind::AlreadyExists => {}
                Err(error) => return Err(error.into()),
            }
        }
        Err(io::Error::new(io::ErrorKind::AlreadyExists, "no free file name").into())
    }

    pub fn save(&self, record: &Record) -> Result<(), Error> {
        let path = self.record_path(&record.transfer);
        let tmp = path.with_extension("tmp");
        let mut file = OpenOptions::new().write(true).create(true).truncate(true).mode(0o600).open(&tmp)?;
        file.write_all(&serde_json::to_vec(record)?)?;
        file.sync_data()?;
        fs::rename(&tmp, &path)?;
        Ok(())
    }

    /// Deletes the record and every part it still names.
    pub fn discard(&self, record: &Record) {
        for file in record.files.iter().filter(|file| file.path.is_none()) {
            remove_quietly(&file.part);
        }
        remove_quietly(&self.record_path(&record.transfer));
    }

    /// Every record on disk; one older than `max_age` seconds is discarded with its parts instead.
    pub fn load(&self, max_age: u64) -> Vec<Record> {
        let Ok(entries) = fs::read_dir(&self.records) else { return Vec::new() };
        let mut records = Vec::new();
        for entry in entries.flatten() {
            let path = entry.path();
            if path.extension().is_none_or(|ext| ext != "json") {
                continue;
            }
            match fs::read(&path).map_err(Error::from).and_then(|bytes| Ok(serde_json::from_slice::<Record>(&bytes)?)) {
                Ok(record) if now().saturating_sub(record.created) <= max_age => records.push(record),
                Ok(record) => {
                    log::info!("transfer {} expired", record.transfer);
                    self.discard(&record);
                }
                Err(error) => {
                    log::warn!("dropping unreadable transfer record {}: {error}", path.display());
                    remove_quietly(&path);
                }
            }
        }
        records
    }

    fn record_path(&self, transfer: &str) -> PathBuf {
        self.records.join(format!("{transfer}.json"))
    }
}

/// Moves the part to `path` unless `path` exists.
fn place(part: &Path, path: &Path) -> io::Result<()> {
    match fs::hard_link(part, path) {
        Ok(()) => fs::remove_file(part),
        // Android's SELinux policy denies apps `link`; a rename that refuses to replace is as safe.
        Err(error) if error.kind() == io::ErrorKind::PermissionDenied => {
            let (cwd, flags) = (rustix::fs::CWD, rustix::fs::RenameFlags::NOREPLACE);
            rustix::fs::renameat_with(cwd, part, cwd, path, flags).map_err(io::Error::from)
        }
        Err(error) => Err(error),
    }
}

pub fn remove_quietly(path: &Path) {
    if let Err(error) = fs::remove_file(path)
        && error.kind() != io::ErrorKind::NotFound
    {
        log::warn!("removing {}: {error}", path.display());
    }
}

impl Record {
    pub fn id(&self) -> Option<TransferId> {
        TransferId::parse_hex(&self.transfer)
    }

    pub fn file_mut(&mut self, id: u64) -> Option<&mut RecordFile> {
        self.files.iter_mut().find(|file| file.id == id)
    }
}

impl Part {
    /// Opens an existing part, cut to `offset`, and re-hashes what it holds: a digest context cannot be saved.
    pub async fn open(path: &Path, offset: u64) -> Result<Self, Error> {
        let flags = rustix::fs::OFlags::NOFOLLOW.bits();
        let file = OpenOptions::new().read(true).write(true).custom_flags(flags.cast_signed()).open(path)?;
        file.set_len(offset)?;
        let mut hasher = digest::Context::new(&digest::SHA256);
        let mut buf = vec![0; REHASH_CHUNK];
        let mut at = 0;
        while at < offset {
            let want = usize::try_from(offset - at).map_or(REHASH_CHUNK, |left| left.min(REHASH_CHUNK));
            let read = file.read_at(&mut buf[..want], at)?;
            if read == 0 {
                return Err(io::Error::from(io::ErrorKind::UnexpectedEof).into());
            }
            hasher.update(&buf[..read]);
            at += read as u64;
            tokio::task::yield_now().await;
        }
        Ok(Self { file, hasher, offset })
    }

    pub fn write(&mut self, bytes: &[u8]) -> Result<(), Error> {
        self.file.write_all_at(bytes, self.offset)?;
        self.hasher.update(bytes);
        self.offset += bytes.len() as u64;
        Ok(())
    }

    /// Flushes to disk and returns the offset that is now durable.
    pub fn sync(&self) -> Result<u64, Error> {
        self.file.sync_data()?;
        Ok(self.offset)
    }

    pub fn offset(&self) -> u64 {
        self.offset
    }

    pub fn digest(self) -> Vec<u8> {
        self.hasher.finish().as_ref().to_vec()
    }
}
