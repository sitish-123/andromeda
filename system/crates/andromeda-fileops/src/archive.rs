//! Archives: list, extract and compress zip, 7z and tar (plain, .gz, .xz, .zst, .bz2). Archives are untrusted input,
//! so every decoder is pure Rust, every entry path is checked before anything is written, and extraction goes to a
//! hidden staging folder in the destination first: a refused or cancelled archive leaves nothing behind.

use std::cell::Cell;
use std::fs::{self, File, OpenOptions};
use std::io::{self, BufReader, BufWriter, Read, Write};
use std::os::unix::ffi::{OsStrExt, OsStringExt};
use std::os::unix::fs::{MetadataExt, OpenOptionsExt, symlink};
use std::path::{Component, Path, PathBuf};
use std::sync::atomic::{AtomicU32, Ordering};
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

use crate::ops::{self, Control, OpError, OpResult, UndoStep};

/// More entries than this is taken for a bomb.
pub const MAX_ENTRIES: u64 = 1_000_000;
/// Free space kept on the destination file system; an archive that would expand into it is a bomb.
const FREE_SPACE_RESERVE: u64 = 256 << 20;
const CHUNK: usize = 1 << 20;
const MAX_LINK_TARGET: u64 = 4096;
/// `ListArchive` reads a tar to its end: past this many decoded bytes or this time it gives up (`TooLarge`).
const LIST_MAX_BYTES: u64 = 16 << 30;
const LIST_MAX_TIME: Duration = Duration::from_secs(30);

/// When a walk stops early: the caller cancelled, or the decoded bytes or time ran out.
struct Limits<'a> {
    stop: &'a dyn Fn() -> bool,
    max_bytes: u64,
    deadline: Option<Instant>,
}

impl Limits<'_> {
    fn check(&self, read: u64) -> Result<(), ArchiveError> {
        if (self.stop)() {
            return Err(ArchiveError::Io(io::Error::from(io::ErrorKind::Interrupted)));
        }
        if read > self.max_bytes || self.deadline.is_some_and(|d| Instant::now() > d) {
            return Err(ArchiveError::TooLarge);
        }
        Ok(())
    }
}

/// Marks an `io::Error` a `Guard` raised, so the walk can turn it back into the `ArchiveError`.
#[derive(Debug)]
struct Stopped(bool); // true: too large

impl std::fmt::Display for Stopped {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{}", if self.0 { "too large" } else { "stopped" })
    }
}

impl std::error::Error for Stopped {}

/// Checks the limits on every read of a decoded stream, also while a decoder skips entries no one reads.
struct Guard<'a, R> {
    inner: R,
    limits: &'a Limits<'a>,
    read: u64,
}

impl<R: Read> Read for Guard<'_, R> {
    fn read(&mut self, buf: &mut [u8]) -> io::Result<usize> {
        match self.limits.check(self.read) {
            Ok(()) => {}
            Err(ArchiveError::TooLarge) => return Err(io::Error::other(Stopped(true))),
            Err(_) => return Err(io::Error::other(Stopped(false))),
        }
        let n = self.inner.read(buf)?;
        self.read += n as u64;
        Ok(n)
    }
}

/// An `io::Error` from a `Guard` back to what stopped it.
fn unguard(e: ArchiveError) -> ArchiveError {
    let stopped = match &e {
        ArchiveError::Io(io) => io.get_ref().and_then(|inner| inner.downcast_ref::<Stopped>()).map(|s| s.0),
        _ => None,
    };
    match stopped {
        Some(true) => ArchiveError::TooLarge,
        Some(false) => ArchiveError::Io(io::Error::from(io::ErrorKind::Interrupted)),
        None => e,
    }
}

static STAGING: AtomicU32 = AtomicU32::new(0);

/// Per decoder (dictionary, 7z header buffer), in KiB. 4x the largest preset dictionary (64 MiB: xz -9, 7-Zip
/// Ultra); the archive header sets it, so without a cap a 200-byte file makes the daemon allocate up to 4 GiB.
const DECODER_MEMORY_KIB: u32 = 256 * 1024;

#[derive(Debug)]
pub enum ArchiveError {
    PasswordNeeded,
    WrongPassword,
    Unsupported,
    /// An entry path that leaves the destination (absolute, "..") or runs through a link.
    Unsafe(String),
    TooLarge,
    /// A decoder would need more than `DECODER_MEMORY_KIB`.
    TooMuchMemory,
    Corrupt(String),
    Io(io::Error),
}

impl std::fmt::Display for ArchiveError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::PasswordNeeded => write!(f, "password needed"),
            Self::WrongPassword => write!(f, "wrong password"),
            Self::Unsupported => write!(f, "not a supported archive"),
            Self::Unsafe(path) => write!(f, "unsafe path in archive: {path}"),
            Self::TooLarge => write!(f, "the archive expands to too many files or more than the free space"),
            Self::TooMuchMemory => {
                write!(f, "the archive needs more than {} MiB of memory to unpack", DECODER_MEMORY_KIB / 1024)
            }
            Self::Corrupt(why) => write!(f, "damaged archive: {why}"),
            Self::Io(e) => write!(f, "{e}"),
        }
    }
}

impl From<io::Error> for ArchiveError {
    fn from(e: io::Error) -> Self {
        // lzma-rust2 reports a block over its memory limit as OutOfMemory, before it allocates.
        if e.kind() == io::ErrorKind::OutOfMemory { Self::TooMuchMemory } else { Self::Io(e) }
    }
}

impl From<zip::result::ZipError> for ArchiveError {
    fn from(e: zip::result::ZipError) -> Self {
        use zip::result::ZipError;
        match e {
            ZipError::Io(e) => Self::Io(e),
            ZipError::InvalidPassword => Self::WrongPassword,
            ZipError::UnsupportedArchive(ZipError::PASSWORD_REQUIRED) => Self::PasswordNeeded,
            other => Self::Corrupt(other.to_string()),
        }
    }
}

impl From<sevenz_rust2::Error> for ArchiveError {
    fn from(e: sevenz_rust2::Error) -> Self {
        use sevenz_rust2::Error;
        match e {
            Error::PasswordRequired => Self::PasswordNeeded,
            Error::MaybeBadPassword(_) => Self::WrongPassword,
            Error::MaxMemLimited { .. } => Self::TooMuchMemory,
            Error::Io(e, _) => e.into(),
            other => Self::Corrupt(other.to_string()),
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Kind {
    File,
    Dir,
    Link,
}

impl Kind {
    pub fn name(self) -> &'static str {
        match self {
            Self::File => "file",
            Self::Dir => "dir",
            Self::Link => "link",
        }
    }
}

#[derive(Clone, Debug)]
pub struct Entry {
    /// Relative, checked: no root, no "." or "..".
    pub path: PathBuf,
    pub kind: Kind,
    pub size: u64,
    /// Seconds since 1970.
    pub mtime: u64,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Codec {
    Plain,
    Gz,
    Xz,
    Zst,
    Bz2,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Format {
    Zip,
    SevenZ,
    Tar(Codec),
}

/// What Compress writes.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum OutFormat {
    Zip,
    SevenZ,
    TarGz,
    TarXz,
    TarZst,
}

impl OutFormat {
    pub fn parse(s: &str) -> Option<Self> {
        match s {
            "zip" => Some(Self::Zip),
            "7z" => Some(Self::SevenZ),
            "tar.gz" => Some(Self::TarGz),
            "tar.xz" => Some(Self::TarXz),
            "tar.zst" => Some(Self::TarZst),
            _ => None,
        }
    }

    pub fn takes_password(self) -> bool {
        matches!(self, Self::Zip | Self::SevenZ)
    }
}

/// "photos.zip.002" for part 2 of "photos.zip" (the 7-Zip volume names, read by 7-Zip, PeaZip and Ark).
fn volume_name(base: &Path, part: usize) -> PathBuf {
    let mut name = base.as_os_str().to_os_string();
    name.push(format!(".{part:03}"));
    PathBuf::from(name)
}

/// One archive file, or the volumes name.001, name.002, … read back to back as one file.
struct Volumes {
    parts: Vec<(File, u64)>,
    len: u64,
    pos: u64,
}

impl Volumes {
    fn open(path: &Path) -> io::Result<Self> {
        let first = File::open(path)?;
        let mut parts = vec![(first.metadata()?.len(), first)];
        if let Some(base) = path.as_os_str().as_bytes().strip_suffix(b".001") {
            let base = Path::new(std::ffi::OsStr::from_bytes(base));
            for part in 2.. {
                let Ok(file) = File::open(volume_name(base, part)) else { break };
                parts.push((file.metadata()?.len(), file));
            }
        }
        let len = parts.iter().map(|(len, _)| len).sum();
        Ok(Self { parts: parts.into_iter().map(|(len, file)| (file, len)).collect(), len, pos: 0 })
    }
}

impl Read for Volumes {
    fn read(&mut self, buf: &mut [u8]) -> io::Result<usize> {
        use std::os::unix::fs::FileExt;
        let mut start = 0;
        for (file, len) in &self.parts {
            if self.pos < start + len {
                let at = self.pos - start;
                let want = usize::try_from((len - at).min(buf.len() as u64)).unwrap_or(buf.len());
                let n = file.read_at(&mut buf[..want], at)?;
                self.pos += n as u64;
                return Ok(n);
            }
            start += len;
        }
        Ok(0)
    }
}

impl io::Seek for Volumes {
    fn seek(&mut self, to: io::SeekFrom) -> io::Result<u64> {
        let pos = match to {
            io::SeekFrom::Start(n) => Some(n),
            io::SeekFrom::End(d) => self.len.checked_add_signed(d),
            io::SeekFrom::Current(d) => self.pos.checked_add_signed(d),
        };
        self.pos = pos.ok_or_else(|| io::Error::from(io::ErrorKind::InvalidInput))?;
        Ok(self.pos)
    }
}

fn detect(path: &Path) -> Result<Format, ArchiveError> {
    let mut head = [0u8; 512];
    let mut file = Volumes::open(path)?;
    let mut n = 0;
    while n < head.len() {
        let got = file.read(&mut head[n..])?;
        if got == 0 {
            break;
        }
        n += got;
    }
    let head = &head[..n];
    Ok(if head.starts_with(b"PK\x03\x04") || head.starts_with(b"PK\x05\x06") || head.starts_with(b"PK\x07\x08") {
        Format::Zip
    } else if head.starts_with(&[0x37, 0x7a, 0xbc, 0xaf, 0x27, 0x1c]) {
        Format::SevenZ
    } else if head.starts_with(&[0x1f, 0x8b]) {
        Format::Tar(Codec::Gz)
    } else if head.starts_with(&[0xfd, b'7', b'z', b'X', b'Z', 0]) {
        Format::Tar(Codec::Xz)
    } else if head.starts_with(&[0x28, 0xb5, 0x2f, 0xfd]) {
        Format::Tar(Codec::Zst)
    } else if head.starts_with(b"BZh") {
        Format::Tar(Codec::Bz2)
    } else if head.len() >= 262 && &head[257..262] == b"ustar" {
        Format::Tar(Codec::Plain)
    } else {
        return Err(ArchiveError::Unsupported);
    })
}

/// A relative path with no root, "." or ".." parts; `None` for the archive root itself ("./").
pub fn checked_path(raw: &[u8]) -> Result<Option<PathBuf>, ArchiveError> {
    let unsafe_path = || ArchiveError::Unsafe(String::from_utf8_lossy(raw).into_owned());
    if raw.first() == Some(&b'/') || raw.contains(&0) {
        return Err(unsafe_path());
    }
    let mut out = PathBuf::new();
    for part in raw.split(|&b| b == b'/') {
        match part {
            b"" | b"." => {}
            b".." => return Err(unsafe_path()),
            name => out.push(std::ffi::OsStr::from_bytes(name)),
        }
    }
    Ok((!out.as_os_str().is_empty()).then_some(out))
}

fn unix_seconds(time: SystemTime) -> u64 {
    time.duration_since(UNIX_EPOCH).map_or(0, |d| d.as_secs())
}

fn zip_mtime(dt: Option<zip::DateTime>) -> u64 {
    let Some(dt) = dt else { return 0 };
    chrono::NaiveDate::from_ymd_opt(i32::from(dt.year()), u32::from(dt.month()), u32::from(dt.day()))
        .and_then(|d| d.and_hms_opt(u32::from(dt.hour()), u32::from(dt.minute()), u32::from(dt.second())))
        .map_or(0, |t| u64::try_from(t.and_utc().timestamp()).unwrap_or(0))
}

/// One archive entry while walking, with its data when the walk asked for it.
struct Item<'a> {
    entry: Entry,
    mode: u32,
    /// Symlink target, or the earlier entry a tar hard link names.
    link: Option<Vec<u8>>,
    hard_link: bool,
    data: &'a mut dyn Read,
}

/// Calls `each` per entry in archive order; `each` returns false to stop. Without `with_data` the readers are empty.
fn walk(
    path: &Path,
    password: &str,
    with_data: bool,
    limits: &Limits<'_>,
    each: &mut dyn FnMut(Item<'_>) -> Result<bool, ArchiveError>,
) -> Result<(), ArchiveError> {
    let result = match detect(path)? {
        Format::Zip => walk_zip(path, password, with_data, each),
        Format::SevenZ => walk_7z(path, password, with_data, limits, each),
        Format::Tar(codec) => walk_tar(path, codec, limits, each),
    };
    result.map_err(unguard)
}

fn walk_zip(
    path: &Path,
    password: &str,
    with_data: bool,
    each: &mut dyn FnMut(Item<'_>) -> Result<bool, ArchiveError>,
) -> Result<(), ArchiveError> {
    let mut zip = zip::ZipArchive::new(BufReader::new(Volumes::open(path)?))?;
    for i in 0..zip.len() {
        let raw_xz = with_data && zip_raw_xz(&mut zip.by_index_raw(i)?)?;
        let mut file = if !with_data || raw_xz {
            zip.by_index_raw(i)?
        } else if password.is_empty() {
            zip.by_index(i)?
        } else {
            zip.by_index_decrypt(i, password.as_bytes())?
        };
        // Some Windows tools write "\\" separators; reading them as "/" also catches "..\\" escapes.
        let raw: Vec<u8> = file.name_raw().iter().map(|&b| if b == b'\\' { b'/' } else { b }).collect();
        let Some(inner) = checked_path(&raw)? else { continue };
        let kind = if file.is_dir() {
            Kind::Dir
        } else if file.is_symlink() {
            Kind::Link
        } else {
            Kind::File
        };
        let entry = Entry { path: inner, kind, size: file.size(), mtime: zip_mtime(file.last_modified()) };
        let mode = file.unix_mode().unwrap_or(0);
        let mut decoded: Box<dyn Read + '_> = if raw_xz {
            let (left, want) = (file.size(), file.crc32());
            let xz = lzma_rust2::XzReader::new_mem_limit(&mut file, false, DECODER_MEMORY_KIB);
            Box::new(ZipChecked { inner: xz, left, crc: flate2::Crc::new(), want })
        } else {
            Box::new(&mut file)
        };
        let link = if kind == Kind::Link && with_data {
            let mut target = Vec::new();
            (&mut decoded).take(MAX_LINK_TARGET).read_to_end(&mut target)?;
            Some(target)
        } else {
            None
        };
        let mut empty = io::empty();
        let data: &mut dyn Read = if with_data && kind == Kind::File { &mut decoded } else { &mut empty };
        if !each(Item { entry, mode, link, hard_link: false, data })? {
            break;
        }
    }
    Ok(())
}

/// zip decodes LZMA and xz entries with no memory limit. An LZMA dictionary over the limit is refused here; true means
/// the caller decodes the raw xz data itself, with the limit. Encrypted LZMA and xz entries cannot be checked before
/// zip decodes them, so they are refused.
fn zip_raw_xz<R: Read>(raw: &mut zip::read::ZipFile<'_, R>) -> Result<bool, ArchiveError> {
    use zip::CompressionMethod;
    let method = raw.compression();
    if method != CompressionMethod::Lzma && method != CompressionMethod::Xz {
        return Ok(false);
    }
    if raw.encrypted() {
        return Err(ArchiveError::Unsupported);
    }
    if method == CompressionMethod::Xz {
        return Ok(true);
    }
    // APPNOTE 5.8.8: 2-byte version, 2-byte properties size, then the LZMA properties byte and a 4-byte dictionary size.
    let mut header = [0; 9];
    raw.read_exact(&mut header)?;
    let dict = u64::from(u32::from_le_bytes([header[5], header[6], header[7], header[8]]));
    // zip passes the declared size to lzma-rust2, which shrinks the dictionary to it up to u64::MAX / 2.
    let size = raw.size();
    let dict = if size <= u64::MAX / 2 { dict.min(size) } else { dict };
    if dict / 1024 > u64::from(DECODER_MEMORY_KIB) { Err(ArchiveError::TooMuchMemory) } else { Ok(false) }
}

/// The size cap and CRC-32 check that zip adds to the entries it decodes, for the xz data this module decodes itself.
struct ZipChecked<R> {
    inner: R,
    left: u64,
    crc: flate2::Crc,
    want: u32,
}

impl<R: Read> Read for ZipChecked<R> {
    fn read(&mut self, buf: &mut [u8]) -> io::Result<usize> {
        let n = self.inner.read(buf)?;
        let damaged = |why: &str| io::Error::new(io::ErrorKind::InvalidData, format!("damaged archive: {why}"));
        if n == 0 && self.left > 0 {
            return Err(damaged("entry shorter than its declared size"));
        }
        self.left = self.left.checked_sub(n as u64).ok_or_else(|| damaged("entry longer than its declared size"))?;
        self.crc.update(&buf[..n]);
        if n > 0 && self.left == 0 && self.crc.sum() != self.want {
            return Err(damaged("entry checksum does not match"));
        }
        Ok(n)
    }
}

fn walk_7z(
    path: &Path,
    password: &str,
    with_data: bool,
    limits: &Limits<'_>,
    each: &mut dyn FnMut(Item<'_>) -> Result<bool, ArchiveError>,
) -> Result<(), ArchiveError> {
    let password = if password.is_empty() { sevenz_rust2::Password::empty() } else { password.into() };
    let mut reader = sevenz_rust2::ArchiveReader::new_with_memory_limit(
        Volumes::open(path)?,
        password,
        DECODER_MEMORY_KIB as usize,
    )?;
    let item = |e: &sevenz_rust2::ArchiveEntry| -> Result<Option<Entry>, ArchiveError> {
        let Some(inner) = checked_path(&e.name().replace('\\', "/").into_bytes())? else { return Ok(None) };
        let kind = if e.is_directory() { Kind::Dir } else { Kind::File };
        let mtime = unix_seconds(SystemTime::from(e.last_modified_date()));
        Ok(Some(Entry { path: inner, kind, size: e.size(), mtime }))
    };
    if !with_data {
        for e in &reader.archive().files {
            if let Some(entry) = item(e)?
                && !each(Item { entry, mode: 0, link: None, hard_link: false, data: &mut io::empty() })?
            {
                break;
            }
        }
        return Ok(());
    }
    let mut failure = None;
    reader.for_each_entries(|e, data| {
        let result = limits.check(0).and_then(|()| item(e)).and_then(|entry| match entry {
            Some(entry) => each(Item { entry, mode: 0, link: None, hard_link: false, data: &mut *data }),
            None => Ok(true),
        });
        // Solid blocks decode in order: an entry left unread would leak into the next one. The drain stops at the
        // limits, and a stopped walk does not drain at all.
        let result = result.and_then(|go_on| {
            if go_on {
                let mut guard = Guard { inner: data, limits, read: 0 };
                io::copy(&mut guard, &mut io::sink()).map_err(|e| unguard(e.into()))?;
            }
            Ok(go_on)
        });
        match result {
            Ok(go_on) => Ok(go_on),
            Err(e) => {
                failure = Some(e);
                Ok(false)
            }
        }
    })?;
    failure.map_or(Ok(()), Err)
}

fn tar_stream(path: &Path, codec: Codec) -> Result<Box<dyn Read>, ArchiveError> {
    let file = BufReader::new(Volumes::open(path)?);

    Ok(match codec {
        Codec::Plain => Box::new(file),
        Codec::Gz => Box::new(flate2::read::MultiGzDecoder::new(file)),
        Codec::Xz => Box::new(lzma_rust2::XzReader::new_mem_limit(file, true, DECODER_MEMORY_KIB)),
        Codec::Zst => {
            Box::new(ruzstd::decoding::StreamingDecoder::new(file).map_err(|e| ArchiveError::Corrupt(e.to_string()))?)
        }
        Codec::Bz2 => Box::new(bzip2::read::MultiBzDecoder::new(file)),
    })
}

fn walk_tar(
    path: &Path,
    codec: Codec,
    limits: &Limits<'_>,
    each: &mut dyn FnMut(Item<'_>) -> Result<bool, ArchiveError>,
) -> Result<(), ArchiveError> {
    let mut tar = tar::Archive::new(Guard { inner: tar_stream(path, codec)?, limits, read: 0 });
    for entry in tar.entries()? {
        let mut entry = entry?;
        let Some(inner) = checked_path(&entry.path_bytes())? else { continue };
        let header = entry.header();
        let (kind, hard_link) = match header.entry_type() {
            tar::EntryType::Regular | tar::EntryType::Continuous => (Kind::File, false),
            tar::EntryType::Directory => (Kind::Dir, false),
            tar::EntryType::Symlink => (Kind::Link, false),
            tar::EntryType::Link => (Kind::File, true),
            _ => continue,
        };
        let mode = header.mode().unwrap_or(0);
        let mtime = header.mtime().unwrap_or(0);
        let link = entry.link_name_bytes().map(std::borrow::Cow::into_owned);
        let size = entry.size();
        let item = Item { entry: Entry { path: inner, kind, size, mtime }, mode, link, hard_link, data: &mut entry };
        if !each(item)? {
            break;
        }
    }
    Ok(())
}

/// Every entry, in archive order. An encrypted 7z header without the password is `PasswordNeeded`.
pub fn list(path: &Path) -> Result<Vec<Entry>, ArchiveError> {
    let mut out = Vec::new();
    let limits = Limits { stop: &|| false, max_bytes: LIST_MAX_BYTES, deadline: Some(Instant::now() + LIST_MAX_TIME) };
    walk(path, "", false, &limits, &mut |item| {
        out.push(item.entry);
        if out.len() as u64 > MAX_ENTRIES {
            return Err(ArchiveError::TooLarge);
        }
        Ok(true)
    })?;
    Ok(out)
}

/// (files, bytes) for the progress total, when the archive has a cheap index (zip, 7z); tar would need a full read.
pub fn totals(path: &Path) -> (u64, u64) {
    if !matches!(detect(path), Ok(Format::Zip | Format::SevenZ)) {
        return (0, 0);
    }
    list(path).map_or((0, 0), |entries| {
        (entries.len() as u64, entries.iter().filter(|e| e.kind == Kind::File).map(|e| e.size).sum())
    })
}

/// "photos" for photos.tar.gz, photos.zip, photos.7z.
fn stem(archive: &Path) -> std::ffi::OsString {
    let full = archive.file_name().map(|n| n.as_bytes().to_vec()).unwrap_or_default();
    let mut name = full.clone();
    if name.len() > 4 && name.ends_with(b".001") {
        name.truncate(name.len() - 4);
    }
    let lower = name.to_ascii_lowercase();
    let cut = [".tar.gz", ".tar.xz", ".tar.zst", ".tar.bz2", ".tgz", ".txz", ".tbz2", ".zip", ".7z", ".tar"]
        .iter()
        .find(|ext| lower.ends_with(ext.as_bytes()) && lower.len() > ext.len())
        .map_or(name.len(), |ext| name.len() - ext.len());
    // A stem of "." or ".." would make the staging root escape the staging folder.
    if matches!(&name[..cut], b"." | b"..") {
        return std::ffi::OsString::from_vec(full);
    }
    std::ffi::OsString::from_vec(name[..cut].to_vec())
}

/// Removes the staging folder on every way out.
struct Staging(PathBuf);

impl Drop for Staging {
    fn drop(&mut self) {
        if fs::symlink_metadata(&self.0).is_ok_and(|m| m.is_dir()) {
            let _ = fs::remove_dir_all(&self.0);
        } else {
            let _ = fs::remove_file(&self.0);
        }
    }
}

struct Writer<'a> {
    root: PathBuf,
    ctl: &'a dyn Control,
    files: u64,
    bytes: u64,
    entries: u64,
    budget: u64,
    /// Links wait until every file and folder is written; then each is checked against the final tree.
    links: Vec<(PathBuf, Vec<u8>)>,
}

fn io_err(e: ArchiveError) -> io::Error {
    match e {
        ArchiveError::Io(e) => e,
        other => io::Error::other(other.to_string()),
    }
}

impl Writer<'_> {
    fn check(&self) -> Result<(), ArchiveError> {
        if self.ctl.cancelled() { Err(ArchiveError::Io(io::Error::from(io::ErrorKind::Interrupted))) } else { Ok(()) }
    }

    /// Makes every folder of `rel` under the root; an existing file or link on the way is refused.
    fn folders(&self, rel: &Path) -> Result<PathBuf, ArchiveError> {
        let mut at = self.root.clone();
        for part in rel.components() {
            let Component::Normal(name) = part else { return Err(ArchiveError::Unsafe(rel.display().to_string())) };
            at.push(name);
            match fs::symlink_metadata(&at) {
                Ok(meta) if meta.is_dir() => {}
                Ok(_) => return Err(ArchiveError::Unsafe(rel.display().to_string())),
                Err(e) if e.kind() == io::ErrorKind::NotFound => fs::create_dir(&at)?,
                Err(e) => return Err(e.into()),
            }
        }
        Ok(at)
    }

    /// The place for `rel`, its folders made and any earlier entry of the same name removed.
    fn slot(&self, rel: &Path) -> Result<PathBuf, ArchiveError> {
        let parent = self.folders(rel.parent().unwrap_or(Path::new("")))?;
        let target = parent.join(rel.file_name().ok_or_else(|| ArchiveError::Unsafe(rel.display().to_string()))?);
        match fs::symlink_metadata(&target) {
            Ok(meta) if meta.is_dir() => Err(ArchiveError::Unsafe(rel.display().to_string())),
            Ok(_) => Ok(fs::remove_file(&target).map(|()| target)?),
            Err(_) => Ok(target),
        }
    }

    fn write(&mut self, rel: &Path, item: Item<'_>) -> Result<(), ArchiveError> {
        self.check()?;
        self.entries += 1;
        if self.entries > MAX_ENTRIES {
            return Err(ArchiveError::TooLarge);
        }
        match item.entry.kind {
            Kind::Dir => {
                self.folders(rel)?;
            }
            Kind::Link => {
                self.links.push((rel.to_path_buf(), item.link.unwrap_or_default()));
                return Ok(());
            }
            Kind::File if item.hard_link => {
                // The caller maps the source like `rel`; one not extracted, or reached through a link, is left out.
                let Some(source) = item.link.as_deref().and_then(|raw| checked_path(raw).ok().flatten()) else {
                    return Ok(());
                };
                if !self.is_real_file(&source) {
                    return Ok(());
                }
                let slot = self.slot(rel)?;
                let mut from = OpenOptions::new()
                    .read(true)
                    .custom_flags(rustix::fs::OFlags::NOFOLLOW.bits().cast_signed())
                    .open(self.root.join(&source))?;
                self.copy_out(&mut from, &slot, 0o644, item.entry.mtime)?;
            }
            Kind::File => {
                let slot = self.slot(rel)?;
                self.copy_out(item.data, &slot, item.mode, item.entry.mtime)?;
            }
        }
        self.files += 1;
        self.ctl.progress(self.files, self.bytes, rel);
        Ok(())
    }

    /// `rel` is a regular file reached through real folders only (no link on the way).
    fn is_real_file(&self, rel: &Path) -> bool {
        let mut at = self.root.clone();
        let parts: Vec<_> = rel.components().collect();
        for (i, part) in parts.iter().enumerate() {
            let Component::Normal(name) = part else { return false };
            at.push(name);
            let Ok(meta) = fs::symlink_metadata(&at) else { return false };
            let last = i + 1 == parts.len();
            if (last && !meta.is_file()) || (!last && !meta.is_dir()) {
                return false;
            }
        }
        !parts.is_empty()
    }

    /// Makes the waiting links. `separate`: top-level items move on their own, so a link must stay inside its own
    /// top-level item. A target is walked part by part on the written tree: every part before the last must be a
    /// real folder, never a link, so a chain of links cannot climb out.
    fn make_links(&mut self, separate: bool) -> Result<(), ArchiveError> {
        for (rel, target) in std::mem::take(&mut self.links) {
            self.check()?;
            if !self.link_stays_inside(&rel, &target, separate) {
                log::warn!(
                    "archive link {} -> {} leaves the folder; left out",
                    rel.display(),
                    String::from_utf8_lossy(&target)
                );
                continue;
            }
            let slot = self.slot(&rel)?;
            symlink(std::ffi::OsStr::from_bytes(&target), slot)?;
            self.files += 1;
            self.ctl.progress(self.files, self.bytes, &rel);
        }
        Ok(())
    }

    fn link_stays_inside(&self, rel: &Path, target: &[u8], separate: bool) -> bool {
        if target.is_empty() || target.first() == Some(&b'/') || target.contains(&0) {
            return false;
        }
        let top = rel.components().next();
        let mut stack: Vec<std::ffi::OsString> =
            rel.parent().map(|p| p.components().map(|c| c.as_os_str().to_owned()).collect()).unwrap_or_default();
        let parts: Vec<&[u8]> = target.split(|&b| b == b'/').filter(|p| !p.is_empty() && *p != b".").collect();
        for (i, part) in parts.iter().enumerate() {
            if *part == b".." {
                if stack.pop().is_none() {
                    return false;
                }
            } else {
                stack.push(std::ffi::OsStr::from_bytes(part).to_owned());
            }
            if separate && (stack.is_empty() || top.is_none_or(|t| t.as_os_str() != stack[0])) {
                return false;
            }
            if i + 1 < parts.len() && !stack.is_empty() {
                let at: PathBuf = stack.iter().fold(self.root.clone(), |p, n| p.join(n));
                if !fs::symlink_metadata(&at).is_ok_and(|m| m.is_dir()) {
                    return false;
                }
            }
        }
        true
    }

    fn copy_out(&mut self, data: &mut dyn Read, slot: &Path, mode: u32, mtime: u64) -> Result<(), ArchiveError> {
        let perm = if mode & 0o777 == 0 { 0o644 } else { mode & 0o777 };
        let mut out = OpenOptions::new()
            .write(true)
            .create_new(true)
            .mode(perm)
            .custom_flags(rustix::fs::OFlags::NOFOLLOW.bits().cast_signed())
            .open(slot)?;
        let mut buf = vec![0u8; CHUNK];
        loop {
            self.check()?;
            let n = data.read(&mut buf)?;
            if n == 0 {
                break;
            }
            self.bytes += n as u64;
            if self.bytes > self.budget {
                return Err(ArchiveError::TooLarge);
            }
            out.write_all(&buf[..n])?;
        }
        if mtime > 0 {
            let _ = out.set_modified(UNIX_EPOCH + Duration::from_secs(mtime));
        }
        Ok(())
    }
}

fn free_space(dir: &Path) -> u64 {
    rustix::fs::statvfs(dir).map_or(u64::MAX, |s| s.f_bavail.saturating_mul(s.f_frsize))
}

fn to_op(path: &Path, e: ArchiveError, ctl: &dyn Control) -> OpError {
    if ctl.cancelled() {
        return OpError::Cancelled;
    }
    OpError::Io(path.to_path_buf(), io_err(e))
}

/// Extracts `inner` (all when empty) into the folder `dest`. All: one top-level item lands as it is, more go into a
/// folder named after the archive. Picked items land by their own name. Name clashes ask like a copy; undo removes
/// what was made.
pub fn extract(
    archive: &Path,
    inner: &[PathBuf],
    dest: &Path,
    password: &str,
    ctl: &dyn Control,
) -> (Vec<UndoStep>, OpResult<()>) {
    let n = STAGING.fetch_add(1, Ordering::Relaxed);
    let staging = dest.join(format!(".andromeda-extract-{}-{n}", std::process::id()));
    if let Err(e) = fs::create_dir(&staging) {
        return (Vec::new(), Err(OpError::Io(staging, e)));
    }
    let staging = Staging(staging);
    let all = inner.is_empty();
    let root = if all { staging.0.join(stem(archive)) } else { staging.0.clone() };
    if let Err(e) = fs::create_dir_all(&root) {
        return (Vec::new(), Err(OpError::Io(root, e)));
    }
    let mut writer = Writer {
        root: root.clone(),
        ctl,
        files: 0,
        bytes: 0,
        entries: 0,
        budget: free_space(dest).saturating_sub(FREE_SPACE_RESERVE),
        links: Vec::new(),
    };
    // Each pick lands under its own name; two picks with one name get a "(copy)" name.
    let mut picks: Vec<(&Path, PathBuf)> = Vec::new();
    for pick in inner {
        let Some(name) = pick.file_name() else { continue };
        let mut landed = PathBuf::from(name);
        for n in 1..u32::MAX {
            if !picks.iter().any(|(_, used)| *used == landed) {
                break;
            }
            landed = free_pick_name(name, n);
        }
        picks.push((pick, landed));
    }
    let map = |path: &Path| -> Option<PathBuf> {
        if all {
            return Some(path.to_path_buf());
        }
        picks.iter().find_map(|(pick, landed)| path.strip_prefix(pick).ok().map(|rest| landed.join(rest)))
    };
    let limits = Limits { stop: &|| ctl.cancelled(), max_bytes: u64::MAX, deadline: None };
    let result = walk(archive, password, true, &limits, &mut |mut item| {
        writer.check()?;
        let Some(rel) = map(&item.entry.path) else { return Ok(true) };
        if item.hard_link {
            let source = item.link.as_deref().and_then(|raw| checked_path(raw).ok().flatten()).and_then(|p| map(&p));
            let Some(source) = source else { return Ok(true) };
            item.link = Some(source.into_os_string().into_vec());
        }
        writer.write(&rel, item)?;
        Ok(true)
    })
    .and_then(|()| {
        // Top-level items move on their own unless several go into the folder named after the archive.
        let mut tops: std::collections::BTreeSet<std::ffi::OsString> =
            fs::read_dir(&root)?.flatten().map(|e| e.file_name()).collect();
        tops.extend(
            writer.links.iter().filter_map(|(rel, _)| rel.components().next()).map(|c| c.as_os_str().to_owned()),
        );
        writer.make_links(!all || tops.len() == 1)
    });
    if let Err(e) = result {
        return (Vec::new(), Err(to_op(archive, e, ctl)));
    }
    let sources: Vec<PathBuf> = match fs::read_dir(&root) {
        Ok(items) => items.flatten().map(|e| e.path()).collect(),
        Err(e) => return (Vec::new(), Err(OpError::Io(root, e))),
    };
    let sources = if all && sources.len() != 1 { vec![root] } else { sources };
    let (steps, result) = ops::move_to(&sources, dest, ctl);
    let undo = steps
        .into_iter()
        .map(|step| match step {
            UndoStep::Moved { to, .. } => UndoStep::Created(to),
            other => other,
        })
        .collect();
    (undo, result)
}

/// "x (copy).txt", "x (copy 2).txt" for the n-th clash of a picked name.
fn free_pick_name(name: &std::ffi::OsStr, n: u32) -> PathBuf {
    let text = name.to_string_lossy();
    let (stem, ext) = match text.rfind('.') {
        Some(dot) if dot > 0 => (&text[..dot], &text[dot..]),
        _ => (&text[..], ""),
    };
    let suffix = if n == 1 { " (copy)".to_owned() } else { format!(" (copy {n})") };
    PathBuf::from(format!("{stem}{suffix}{ext}"))
}

/// Counts progress and stops at cancel while a compressor reads a file.
struct Feed<'a, R> {
    inner: R,
    ctl: &'a dyn Control,
    bytes: &'a Cell<u64>,
}

impl<R: Read> Read for Feed<'_, R> {
    fn read(&mut self, buf: &mut [u8]) -> io::Result<usize> {
        if self.ctl.cancelled() {
            return Err(io::Error::other("cancelled"));
        }
        let n = self.inner.read(buf)?;
        self.bytes.set(self.bytes.get() + n as u64);
        Ok(n)
    }
}

/// ruzstd's encoder unwraps I/O results; these keep its source and drain infallible and keep the first error.
struct Quiet<T> {
    inner: T,
    error: Option<io::Error>,
}

impl<R: Read> Read for Quiet<R> {
    fn read(&mut self, buf: &mut [u8]) -> io::Result<usize> {
        if self.error.is_some() {
            return Ok(0);
        }
        self.inner.read(buf).or_else(|e| {
            self.error = Some(e);
            Ok(0)
        })
    }
}

impl<W: Write> Write for Quiet<W> {
    fn write(&mut self, buf: &[u8]) -> io::Result<usize> {
        if self.error.is_none()
            && let Err(e) = self.inner.write_all(buf)
        {
            self.error = Some(e);
        }
        Ok(buf.len())
    }
    fn flush(&mut self) -> io::Result<()> {
        Ok(())
    }
}

/// Every path under `sources` (links not followed), with its name in the archive: relative to its source's folder.
fn gather(sources: &[PathBuf]) -> io::Result<Vec<(PathBuf, String, fs::Metadata)>> {
    fn walk_dir(path: &Path, name: &str, out: &mut Vec<(PathBuf, String, fs::Metadata)>) -> io::Result<()> {
        let meta = fs::symlink_metadata(path)?;
        let is_dir = meta.is_dir();
        out.push((path.to_path_buf(), name.to_owned(), meta));
        if is_dir {
            let mut children: Vec<_> = fs::read_dir(path)?.flatten().map(|e| e.path()).collect();
            children.sort();
            for child in children {
                let child_name = format!("{name}/{}", child.file_name().unwrap_or_default().to_string_lossy());
                walk_dir(&child, &child_name, out)?;
            }
        }
        Ok(())
    }
    let mut out = Vec::new();
    for source in sources {
        let name = source.file_name().ok_or_else(|| io::Error::from(io::ErrorKind::InvalidInput))?;
        walk_dir(source, &name.to_string_lossy(), &mut out)?;
    }
    Ok(out)
}

/// Writes `sources` into a new archive at `destination` (a taken name gets a "(copy)" name). The archive is written
/// to a hidden part file and renamed at the end, so a cancel or error leaves nothing. Undo removes the archive.
/// A `volume` size above 0 cuts an archive larger than it into destination.001, .002, … of that size.
pub fn compress(
    sources: &[PathBuf],
    destination: &Path,
    format: OutFormat,
    password: &str,
    volume: u64,
    ctl: &dyn Control,
) -> (Vec<UndoStep>, OpResult<()>) {
    let parent = destination.parent().unwrap_or(Path::new("/"));
    let n = STAGING.fetch_add(1, Ordering::Relaxed);
    let part = parent.join(format!(".andromeda-compress-{}-{n}.part", std::process::id()));
    let guard = Staging(part.clone());
    let bytes = Cell::new(0u64);
    let result = gather(sources).and_then(|items| {
        let out = BufWriter::new(OpenOptions::new().write(true).create_new(true).mode(0o644).open(&part)?);
        match format {
            OutFormat::Zip => write_zip(out, &items, password, ctl, &bytes),
            OutFormat::SevenZ => write_7z(out, &items, password, ctl, &bytes),
            OutFormat::TarGz => {
                let gz = flate2::write::GzEncoder::new(out, flate2::Compression::default());
                write_tar(gz, &items, ctl, &bytes)?.finish()?.flush()
            }
            OutFormat::TarXz => {
                let xz = lzma_rust2::XzWriter::new(out, lzma_rust2::XzOptions::with_preset(6))?;
                write_tar(xz, &items, ctl, &bytes)?.finish()?.flush()
            }
            OutFormat::TarZst => {
                let tar_part = part.with_extension("tar");
                // create_new: never follows or truncates a file someone placed at the name; the guard owns it only after.
                let plain =
                    BufWriter::new(OpenOptions::new().write(true).create_new(true).mode(0o600).open(&tar_part)?);
                let tar_guard = Staging(tar_part.clone());
                write_tar(plain, &items, ctl, &bytes)?.flush()?;
                let mut source = Quiet { inner: BufReader::new(File::open(&tar_guard.0)?), error: None };
                let mut drain = Quiet { inner: out, error: None };
                ruzstd::encoding::compress(&mut source, &mut drain, ruzstd::encoding::CompressionLevel::Fastest);
                if let Some(e) = source.error.or(drain.error) {
                    return Err(e);
                }
                drain.inner.flush()
            }
        }
    });
    if let Err(e) = result {
        let e = if ctl.cancelled() { OpError::Cancelled } else { OpError::Io(destination.to_path_buf(), e) };
        return (Vec::new(), Err(e));
    }
    let volumes = match split_part(&part, volume, ctl) {
        Ok(volumes) => volumes,
        Err(e) => {
            let e = if ctl.cancelled() { OpError::Cancelled } else { OpError::Io(destination.to_path_buf(), e) };
            return (Vec::new(), Err(e));
        }
    };
    if !volumes.is_empty() {
        let staged: Vec<PathBuf> = std::iter::once(part.clone()).chain(volumes.iter().map(|v| v.0.clone())).collect();
        let name = destination.file_name().unwrap_or_default();
        for attempt in 0..100 {
            let base =
                if attempt == 0 { destination.to_path_buf() } else { parent.join(free_pick_name(name, attempt)) };
            let finals: Vec<PathBuf> = (1..=staged.len()).map(|k| volume_name(&base, k)).collect();
            match rename_all(&staged, &finals) {
                Ok(()) => {
                    std::mem::forget(guard);
                    volumes.into_iter().for_each(std::mem::forget);
                    ctl.progress(1, bytes.get(), &finals[0]);
                    return (finals.into_iter().map(UndoStep::Created).collect(), Ok(()));
                }
                Err(e) if e.kind() == io::ErrorKind::AlreadyExists => {}
                Err(e) => return (Vec::new(), Err(OpError::Io(finals[0].clone(), e))),
            }
        }
        return (Vec::new(), Err(OpError::Io(destination.to_path_buf(), io::ErrorKind::AlreadyExists.into())));
    }
    // NOREPLACE: a file made at the target meanwhile is never replaced; the archive takes the next free name.
    let mut target = destination.to_path_buf();
    for _ in 0..100 {
        match rustix::fs::renameat_with(
            rustix::fs::CWD,
            &part,
            rustix::fs::CWD,
            &target,
            rustix::fs::RenameFlags::NOREPLACE,
        ) {
            Ok(()) => break,
            Err(rustix::io::Errno::EXIST) => target = ops::free_name(destination),
            Err(e) => return (Vec::new(), Err(OpError::Io(target, e.into()))),
        }
    }
    if fs::symlink_metadata(&part).is_ok() {
        return (Vec::new(), Err(OpError::Io(target, io::ErrorKind::AlreadyExists.into())));
    }
    std::mem::forget(guard);
    ctl.progress(1, bytes.get(), &target);
    (vec![UndoStep::Created(target)], Ok(()))
}

/// Cuts the finished archive `part` into `size` volumes from its end, so the disk holds at most one volume more than
/// the archive. Returns the staged parts 2..n in order; `part` keeps part 1. An archive that fits stays whole.
fn split_part(part: &Path, size: u64, ctl: &dyn Control) -> io::Result<Vec<Staging>> {
    use std::io::Seek;
    let file = OpenOptions::new().read(true).write(true).open(part)?;
    let len = file.metadata()?.len();
    if size == 0 || len <= size {
        return Ok(Vec::new());
    }
    let mut staged = Vec::new();
    for k in (1..len.div_ceil(size)).rev() {
        if ctl.cancelled() {
            return Err(io::Error::other("cancelled"));
        }
        let path = volume_name(part, usize::try_from(k + 1).unwrap_or(usize::MAX));
        let guard = Staging(path.clone());
        let mut out = OpenOptions::new().write(true).create_new(true).mode(0o644).open(&path)?;
        let mut from = &file;
        from.seek(io::SeekFrom::Start(k * size))?;
        io::copy(&mut from.take(len.min((k + 1) * size) - k * size), &mut out)?;
        file.set_len(k * size)?;
        staged.push(guard);
    }
    staged.reverse();
    Ok(staged)
}

/// Renames every `from` to its `to` without replacing anything; on a taken name the done ones go back.
fn rename_all(from: &[PathBuf], to: &[PathBuf]) -> io::Result<()> {
    use rustix::fs::{CWD, RenameFlags, renameat_with};
    for (i, (a, b)) in from.iter().zip(to).enumerate() {
        if let Err(e) = renameat_with(CWD, a, CWD, b, RenameFlags::NOREPLACE) {
            for (a, b) in from[..i].iter().zip(&to[..i]) {
                let _ = renameat_with(CWD, b, CWD, a, RenameFlags::NOREPLACE);
            }
            return Err(e.into());
        }
    }
    Ok(())
}

fn zip_time(meta: &fs::Metadata) -> zip::DateTime {
    use chrono::{Datelike, Timelike};
    chrono::DateTime::from_timestamp(meta.mtime(), 0)
        .and_then(|t| {
            let (date, time) = (t.date_naive(), t.time());
            zip::DateTime::from_date_and_time(
                u16::try_from(date.year()).ok()?,
                u8::try_from(date.month()).ok()?,
                u8::try_from(date.day()).ok()?,
                u8::try_from(time.hour()).ok()?,
                u8::try_from(time.minute()).ok()?,
                u8::try_from(time.second()).ok()?,
            )
            .ok()
        })
        .unwrap_or_default()
}

fn write_zip<W: Write + io::Seek>(
    out: W,
    items: &[(PathBuf, String, fs::Metadata)],
    password: &str,
    ctl: &dyn Control,
    bytes: &Cell<u64>,
) -> io::Result<()> {
    use zip::write::SimpleFileOptions;
    let mut zip = zip::ZipWriter::new(out);
    for (i, (path, name, meta)) in items.iter().enumerate() {
        let mut options = SimpleFileOptions::default()
            .compression_method(zip::CompressionMethod::Deflated)
            .unix_permissions(meta.mode() & 0o777)
            .last_modified_time(zip_time(meta))
            .large_file(meta.len() >= u64::from(u32::MAX));
        if !password.is_empty() {
            options = options.with_aes_encryption(zip::AesMode::Aes256, password);
        }
        if meta.is_dir() {
            zip.add_directory(format!("{name}/"), options)?;
        } else if meta.is_symlink() {
            zip.add_symlink(name.clone(), fs::read_link(path)?.to_string_lossy(), options)?;
        } else if meta.is_file() {
            zip.start_file(name.clone(), options)?;
            io::copy(&mut Feed { inner: File::open(path)?, ctl, bytes }, &mut zip)?;
        }
        ctl.progress(i as u64 + 1, bytes.get(), path);
    }
    zip.finish()?.flush()
}

fn write_7z<W: Write + io::Seek>(
    out: W,
    items: &[(PathBuf, String, fs::Metadata)],
    password: &str,
    ctl: &dyn Control,
    bytes: &Cell<u64>,
) -> io::Result<()> {
    use sevenz_rust2::encoder_options::AesEncoderOptions;
    use sevenz_rust2::{ArchiveEntry, ArchiveWriter, EncoderMethod};
    let to_io = |e: sevenz_rust2::Error| io::Error::other(e.to_string());
    let mut sz = ArchiveWriter::new(out).map_err(to_io)?;
    if !password.is_empty() {
        sz.set_content_methods(vec![
            AesEncoderOptions::new(sevenz_rust2::Password::new(password)).into(),
            EncoderMethod::LZMA2.into(),
        ]);
        // ListArchive takes no password, so names stay readable as in a password zip.
        sz.set_encrypt_header(false);
    }
    for (i, (path, name, meta)) in items.iter().enumerate() {
        // 7z has no portable symlink entry; links are left out.
        if meta.is_dir() {
            sz.push_archive_entry::<File>(ArchiveEntry::from_path(path, name.clone()), None).map_err(to_io)?;
        } else if meta.is_file() {
            let feed = Feed { inner: File::open(path)?, ctl, bytes };
            sz.push_archive_entry(ArchiveEntry::from_path(path, name.clone()), Some(feed)).map_err(to_io)?;
        }
        if ctl.cancelled() {
            return Err(io::Error::other("cancelled"));
        }
        ctl.progress(i as u64 + 1, bytes.get(), path);
    }
    sz.finish()?.flush()
}

fn write_tar<W: Write>(
    out: W,
    items: &[(PathBuf, String, fs::Metadata)],
    ctl: &dyn Control,
    bytes: &Cell<u64>,
) -> io::Result<W> {
    let mut tar = tar::Builder::new(out);
    tar.follow_symlinks(false);
    for (i, (path, name, meta)) in items.iter().enumerate() {
        let mut header = tar::Header::new_gnu();
        header.set_metadata(meta);
        if meta.is_dir() {
            header.set_size(0);
            tar.append_data(&mut header, name, io::empty())?;
        } else if meta.is_symlink() {
            header.set_size(0);
            tar.append_link(&mut header, name, fs::read_link(path)?)?;
        } else if meta.is_file() {
            tar.append_data(&mut header, name, Feed { inner: File::open(path)?, ctl, bytes })?;
        }
        ctl.progress(i as u64 + 1, bytes.get(), path);
    }
    tar.into_inner()
}

#[cfg(test)]
#[allow(clippy::expect_used, clippy::unwrap_used)]
mod tests {
    use super::*;

    #[test]
    fn an_archive_that_declares_a_huge_dictionary_fails_before_it_allocates() {
        // `xz --lzma2=dict=1536MiB` of a tar with one 5-byte file.
        const TAR_XZ: &[u8] = &[
            0xfd, 0x37, 0x7a, 0x58, 0x5a, 0x00, 0x00, 0x04, 0xe6, 0xd6, 0xb4, 0x46, 0x04, 0xc0, 0x77, 0x80, 0x50, 0x21,
            0x01, 0x25, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4f, 0x16, 0xf3, 0xcf, 0xe0, 0x27, 0xff, 0x00,
            0x6f, 0x5d, 0x00, 0x34, 0x0b, 0x8a, 0x87, 0xc4, 0x0e, 0xf2, 0x97, 0xa4, 0xf8, 0x75, 0x3d, 0xc5, 0xab, 0x01,
            0x8d, 0xdb, 0x74, 0xe2, 0x92, 0xa8, 0x50, 0x8a, 0xd5, 0x8f, 0xaa, 0xa4, 0x36, 0xb6, 0xe0, 0xb7, 0x08, 0x12,
            0x7e, 0x85, 0x3c, 0x79, 0x3d, 0x9a, 0x50, 0xa0, 0x7b, 0x54, 0xc8, 0x3d, 0x10, 0x26, 0x82, 0x47, 0x9e, 0xca,
            0xfc, 0x81, 0x54, 0xe3, 0x61, 0x24, 0x95, 0x37, 0x69, 0x8e, 0xba, 0x70, 0x66, 0x76, 0xa5, 0xf3, 0x79, 0x4e,
            0x20, 0xf5, 0xf4, 0x4a, 0x86, 0x59, 0x58, 0xb9, 0x19, 0xd5, 0x02, 0xba, 0x88, 0x7f, 0x57, 0x13, 0x29, 0x70,
            0x00, 0x3f, 0x9f, 0x19, 0xbd, 0xac, 0x35, 0xdb, 0x4c, 0xc3, 0xdc, 0x0b, 0x23, 0x4f, 0xff, 0xe0, 0xbd, 0x62,
            0x83, 0x60, 0x72, 0xd4, 0x00, 0x00, 0x00, 0x00, 0x9e, 0xc6, 0x1a, 0xaa, 0x49, 0x1f, 0xf5, 0x8e, 0x00, 0x01,
            0x93, 0x01, 0x80, 0x50, 0x00, 0x00, 0x0b, 0x01, 0x22, 0xe4, 0xb1, 0xc4, 0x67, 0xfb, 0x02, 0x00, 0x00, 0x00,
            0x00, 0x04, 0x59, 0x5a,
        ];
        // `7z a -mhc=off -m0=lzma2` of one 5-byte file, its LZMA2 dictionary byte set to 1536 MiB and CRCs fixed.
        const SEVEN_Z: &[u8] = &[
            0x37, 0x7a, 0xbc, 0xaf, 0x27, 0x1c, 0x00, 0x04, 0x49, 0xae, 0x61, 0xc7, 0x09, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x52, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x32, 0xf1, 0x7e, 0x33, 0x01, 0x00, 0x04, 0x68,
            0x65, 0x6c, 0x6c, 0x6f, 0x00, 0x01, 0x04, 0x06, 0x00, 0x01, 0x09, 0x09, 0x00, 0x07, 0x0b, 0x01, 0x00, 0x01,
            0x21, 0x21, 0x01, 0x25, 0x0c, 0x05, 0x00, 0x08, 0x0a, 0x01, 0x86, 0xa6, 0x10, 0x36, 0x00, 0x00, 0x05, 0x01,
            0x19, 0x0c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x11, 0x0d, 0x00, 0x68,
            0x00, 0x2e, 0x00, 0x74, 0x00, 0x78, 0x00, 0x74, 0x00, 0x00, 0x00, 0x14, 0x0a, 0x01, 0x00, 0x8a, 0x6d, 0x22,
            0x52, 0xe3, 0x71, 0xdd, 0x01, 0x15, 0x06, 0x01, 0x00, 0x20, 0x80, 0xa4, 0x81, 0x00, 0x00,
        ];
        // APPNOTE 5.8.8 LZMA header: version, properties size 5, properties byte, 1536 MiB dictionary.
        let lzma = [9, 20, 5, 0, 0x5d, 0, 0, 0, 0x60, 0, 0, 0, 0, 0, 0];
        let zip_lzma = one_entry_zip(14, 1 << 30, 0, &lzma);
        let zip_xz = one_entry_zip(95, 10240, 0, TAR_XZ);
        let root = tempfile::tempdir().unwrap();
        for (name, bytes) in [
            ("big-dict.tar.xz", TAR_XZ),
            ("big-dict.7z", SEVEN_Z),
            ("big-dict-lzma.zip", &zip_lzma),
            ("big-dict-xz.zip", &zip_xz),
        ] {
            let archive = root.path().join(name);
            fs::write(&archive, bytes).unwrap();
            let result = read_all(&archive);
            assert!(matches!(result, Err(ArchiveError::TooMuchMemory)), "{name}: {result:?}");
        }
    }

    /// Reads every entry of `archive` to its end.
    fn read_all(archive: &Path) -> Result<(), ArchiveError> {
        let limits = Limits { stop: &|| false, max_bytes: u64::MAX, deadline: None };
        walk(archive, "", true, &limits, &mut |item| {
            io::copy(item.data, &mut io::sink())?;
            Ok(true)
        })
    }

    #[test]
    fn a_zip_xz_entry_must_match_its_declared_size_and_checksum() {
        let text = b"hello world";
        let mut xz = lzma_rust2::XzWriter::new(Vec::new(), lzma_rust2::XzOptions::with_preset(0)).unwrap();
        xz.write_all(text).unwrap();
        let xz = xz.finish().unwrap();
        let mut crc = flate2::Crc::new();
        crc.update(text);
        let (crc, size) = (crc.sum(), u32::try_from(text.len()).unwrap());
        let root = tempfile::tempdir().unwrap();
        for (name, size, crc, good) in
            [("good", size, crc, true), ("crc", size, crc ^ 1, false), ("short", size - 6, crc, false)]
        {
            let archive = root.path().join(format!("{name}.zip"));
            fs::write(&archive, one_entry_zip(95, size, crc, &xz)).unwrap();
            let result = read_all(&archive);
            assert_eq!(result.is_ok(), good, "{name}: {result:?}");
        }
    }

    #[test]
    fn a_zip_lzma_entry_with_a_big_dictionary_but_a_small_size_extracts() {
        let text = b"hello world";
        let mut lzma =
            lzma_rust2::LzmaWriter::new_no_header(Vec::new(), &lzma_rust2::LzmaOptions::with_preset(0), false).unwrap();
        let props = lzma.props();
        lzma.write_all(text).unwrap();
        // APPNOTE 5.8.8 LZMA header, then a declared 1536 MiB dictionary.
        let mut data = vec![9, 20, 5, 0, props, 0, 0, 0, 0x60];
        data.extend(lzma.finish().unwrap());
        let mut crc = flate2::Crc::new();
        crc.update(text);
        let root = tempfile::tempdir().unwrap();
        let archive = root.path().join("small.zip");
        fs::write(&archive, one_entry_zip(14, u32::try_from(text.len()).unwrap(), crc.sum(), &data)).unwrap();
        let dest = root.path().join("out");
        fs::create_dir(&dest).unwrap();
        extract(&archive, &[], &dest, "", &Quietly).1.unwrap();
        assert_eq!(fs::read(dest.join("a")).unwrap(), text);
    }

    /// A zip with one file "a" whose data is already compressed with `method`.
    fn one_entry_zip(method: u16, size: u32, crc: u32, data: &[u8]) -> Vec<u8> {
        let (packed, dos_date) = (u32::try_from(data.len()).unwrap(), 0x21u16);
        let mut out = 0x0403_4b50u32.to_le_bytes().to_vec();
        for v in [20u16, 0, method, 0, dos_date] {
            out.extend(v.to_le_bytes());
        }
        for v in [crc, packed, size] {
            out.extend(v.to_le_bytes());
        }
        out.extend([1u8, 0, 0, 0, b'a']);
        out.extend(data);
        let central = u32::try_from(out.len()).unwrap();
        out.extend(0x0201_4b50u32.to_le_bytes());
        for v in [0x031eu16, 20, 0, method, 0, dos_date] {
            out.extend(v.to_le_bytes());
        }
        for v in [crc, packed, size] {
            out.extend(v.to_le_bytes());
        }
        for v in [1u16, 0, 0, 0, 0] {
            out.extend(v.to_le_bytes());
        }
        for v in [0o100_644u32 << 16, 0] {
            out.extend(v.to_le_bytes());
        }
        out.push(b'a');
        let central_size = u32::try_from(out.len()).unwrap() - central;
        out.extend(0x0605_4b50u32.to_le_bytes());
        for v in [0u16, 0, 1, 1] {
            out.extend(v.to_le_bytes());
        }
        for v in [central_size, central] {
            out.extend(v.to_le_bytes());
        }
        out.extend([0u8, 0]);
        out
    }

    struct Quietly;

    impl Control for Quietly {
        fn cancelled(&self) -> bool {
            false
        }
        fn progress(&self, _: u64, _: u64, _: &Path) {}
        fn conflict(&self, _: &Path, _: &Path) -> Option<(ops::Resolution, bool)> {
            Some((ops::Resolution::KeepBoth, false))
        }
    }

    fn tree(root: &Path) -> Vec<PathBuf> {
        let mut out = Vec::new();
        for item in gather(&[root.to_path_buf()]).expect("walk") {
            out.push(item.0.strip_prefix(root).expect("inside").to_path_buf());
        }
        out.sort();
        out
    }

    /// docs/a.txt, docs/sub/b.txt
    fn sample_zip(path: &Path, password: Option<&str>) {
        use zip::write::SimpleFileOptions;
        let mut zip = zip::ZipWriter::new(File::create(path).expect("create"));
        let mut options = SimpleFileOptions::default();
        if let Some(pw) = password {
            options = options.with_aes_encryption(zip::AesMode::Aes256, pw);
        }
        zip.add_directory("docs/", options).expect("dir");
        zip.start_file("docs/a.txt", options).expect("a");
        zip.write_all(b"alpha").expect("write");
        zip.start_file("docs/sub/b.txt", options).expect("b");
        zip.write_all(b"beta").expect("write");
        zip.finish().expect("finish");
    }

    #[test]
    fn list_shows_every_entry() {
        let root = tempfile::tempdir().expect("tmp");
        let zip = root.path().join("docs.zip");
        sample_zip(&zip, None);
        let entries = list(&zip).expect("list");
        let names: Vec<_> = entries.iter().map(|e| (e.path.clone(), e.kind, e.size)).collect();
        assert_eq!(
            names,
            vec![
                (PathBuf::from("docs"), Kind::Dir, 0),
                (PathBuf::from("docs/a.txt"), Kind::File, 5),
                (PathBuf::from("docs/sub/b.txt"), Kind::File, 4),
            ]
        );
    }

    #[test]
    fn extract_all_with_one_top_folder_lands_as_is_and_undo_removes_it() {
        let root = tempfile::tempdir().expect("tmp");
        let zip = root.path().join("docs.zip");
        sample_zip(&zip, None);
        let dest = root.path().join("out");
        fs::create_dir(&dest).expect("mkdir");
        let (undo, result) = extract(&zip, &[], &dest, "", &Quietly);
        result.expect("extract");
        assert_eq!(fs::read_to_string(dest.join("docs/sub/b.txt")).expect("b"), "beta");
        assert_eq!(fs::read_dir(&dest).expect("ls").count(), 1, "no staging folder is left");
        assert!(matches!(&undo[..], [UndoStep::Created(p)] if *p == dest.join("docs")));
    }

    #[test]
    fn extract_all_with_many_top_items_goes_into_a_folder_named_after_the_archive() {
        let root = tempfile::tempdir().expect("tmp");
        let src = root.path().join("src");
        fs::create_dir_all(src.join("d")).expect("mkdir");
        fs::write(src.join("one.txt"), "1").expect("write");
        fs::write(src.join("d/two.txt"), "2").expect("write");
        let archive = root.path().join("bundle.tar.gz");
        let (_, result) = compress(&[src.join("one.txt"), src.join("d")], &archive, OutFormat::TarGz, "", 0, &Quietly);
        result.expect("compress");
        let dest = root.path().join("out");
        fs::create_dir(&dest).expect("mkdir");
        extract(&archive, &[], &dest, "", &Quietly).1.expect("extract");
        assert_eq!(tree(&dest.join("bundle")), vec![PathBuf::new(), "d".into(), "d/two.txt".into(), "one.txt".into()]);
    }

    #[test]
    fn an_archive_named_dot_or_dot_dot_extracts_into_a_folder_with_its_full_name() {
        for name in ["..tar.gz", "...tar.gz", "...001"] {
            let root = tempfile::tempdir().expect("tmp");
            let src = root.path().join("src");
            fs::create_dir(&src).expect("mkdir");
            fs::write(src.join("one.txt"), "1").expect("write");
            fs::write(src.join("two.txt"), "2").expect("write");
            let archive = root.path().join(name);
            let (_, result) =
                compress(&[src.join("one.txt"), src.join("two.txt")], &archive, OutFormat::TarGz, "", 0, &Quietly);
            result.expect("compress");
            let dest = root.path().join("out");
            fs::create_dir(&dest).expect("mkdir");
            extract(&archive, &[], &dest, "", &Quietly).1.expect("extract");
            assert_eq!(tree(&dest.join(name)), vec![PathBuf::new(), "one.txt".into(), "two.txt".into()], "{name}");
        }
    }

    #[test]
    fn extract_one_lands_by_its_own_name() {
        let root = tempfile::tempdir().expect("tmp");
        let zip = root.path().join("docs.zip");
        sample_zip(&zip, None);
        let dest = root.path().join("out");
        fs::create_dir(&dest).expect("mkdir");
        extract(&zip, &[PathBuf::from("docs/sub/b.txt")], &dest, "", &Quietly).1.expect("extract");
        assert_eq!(tree(&dest), vec![PathBuf::new(), "b.txt".into()]);
    }

    #[test]
    fn a_zip_slip_entry_is_refused_and_nothing_is_written() {
        use zip::write::SimpleFileOptions;
        let root = tempfile::tempdir().expect("tmp");
        let zip_path = root.path().join("evil.zip");
        let mut zip = zip::ZipWriter::new(File::create(&zip_path).expect("create"));
        zip.start_file("good.txt", SimpleFileOptions::default()).expect("good");
        zip.write_all(b"ok").expect("write");
        zip.start_file("../escaped.txt", SimpleFileOptions::default()).expect("evil");
        zip.write_all(b"pwned").expect("write");
        zip.finish().expect("finish");
        let dest = root.path().join("out");
        fs::create_dir(&dest).expect("mkdir");
        let (undo, result) = extract(&zip_path, &[], &dest, "", &Quietly);
        assert!(result.is_err(), "the archive is refused");
        assert!(undo.is_empty());
        assert!(!root.path().join("escaped.txt").exists());
        assert_eq!(fs::read_dir(&dest).expect("ls").count(), 0, "nothing lands, no staging folder is left");
        assert!(matches!(list(&zip_path), Err(ArchiveError::Unsafe(_))));
    }

    struct CancelledNow;

    impl Control for CancelledNow {
        fn cancelled(&self) -> bool {
            true
        }
        fn progress(&self, _: u64, _: u64, _: &Path) {}
        fn conflict(&self, _: &Path, _: &Path) -> Option<(ops::Resolution, bool)> {
            None
        }
    }

    fn tar_link(tar: &mut tar::Builder<File>, kind: tar::EntryType, name: &str, target: &str) {
        let mut header = tar::Header::new_gnu();
        header.set_entry_type(kind);
        header.set_size(0);
        header.set_mode(0o777);
        tar.append_link(&mut header, name, target).expect("link");
    }

    /// Every path under `dir`, links not followed.
    fn all_under(dir: &Path) -> Vec<PathBuf> {
        let mut out = Vec::new();
        let mut todo = vec![dir.to_path_buf()];
        while let Some(at) = todo.pop() {
            for entry in fs::read_dir(&at).expect("ls").flatten() {
                if entry.file_type().expect("type").is_dir() {
                    todo.push(entry.path());
                }
                out.push(entry.path());
            }
        }
        out
    }

    #[test]
    fn a_chain_of_links_cannot_climb_out_or_leak_a_file() {
        let root = tempfile::tempdir().expect("tmp");
        fs::write(root.path().join("secret.txt"), "top secret").expect("write");
        let archive = root.path().join("evil.tar");
        let mut tar = tar::Builder::new(File::create(&archive).expect("create"));
        let mut dir = tar::Header::new_gnu();
        dir.set_entry_type(tar::EntryType::Directory);
        dir.set_size(0);
        dir.set_mode(0o755);
        tar.append_data(&mut dir, "a/b/c", io::empty()).expect("dir");
        // Each target reads as inside; together they reach the folder above the destination.
        tar_link(&mut tar, tar::EntryType::Symlink, "a/b/c/up", "../..");
        tar_link(&mut tar, tar::EntryType::Symlink, "esc", "a/b/c/up/../../../..");
        tar_link(&mut tar, tar::EntryType::Link, "stolen", "esc/secret.txt");
        tar.into_inner().expect("finish");
        let dest = root.path().join("out");
        fs::create_dir(&dest).expect("mkdir");
        let _ = extract(&archive, &[], &dest, "", &Quietly);
        let dest_real = fs::canonicalize(&dest).expect("dest");
        for path in all_under(&dest) {
            if let Ok(real) = fs::canonicalize(&path) {
                assert!(real.starts_with(&dest_real), "{} resolves outside: {}", path.display(), real.display());
            }
            assert_ne!(
                fs::read_to_string(&path).ok().as_deref(),
                Some("top secret"),
                "{} leaks the file",
                path.display()
            );
        }
    }

    #[test]
    fn a_link_to_a_sibling_is_left_out_when_the_top_item_moves_alone() {
        let root = tempfile::tempdir().expect("tmp");
        let archive = root.path().join("one.tar");
        let mut tar = tar::Builder::new(File::create(&archive).expect("create"));
        let mut file = tar::Header::new_gnu();
        file.set_size(2);
        file.set_mode(0o644);
        tar.append_data(&mut file, "docs/a.txt", &b"hi"[..]).expect("file");
        tar_link(&mut tar, tar::EntryType::Symlink, "docs/inside", "a.txt");
        tar_link(&mut tar, tar::EntryType::Symlink, "docs/l", "../x");
        tar.into_inner().expect("finish");
        let dest = root.path().join("out");
        fs::create_dir(&dest).expect("mkdir");
        extract(&archive, &[], &dest, "", &Quietly).1.expect("extract");
        assert_eq!(fs::read_to_string(dest.join("docs/inside")).expect("inside link"), "hi");
        assert!(fs::symlink_metadata(dest.join("docs/l")).is_err(), "a link to dest/x is left out");
    }

    #[test]
    fn a_cancel_during_a_big_7z_entry_ends_quickly() {
        use sevenz_rust2::{ArchiveEntry, ArchiveWriter, EncoderMethod};
        let root = tempfile::tempdir().expect("tmp");
        let archive = root.path().join("big.7z");
        let mut sz = ArchiveWriter::new(File::create(&archive).expect("create")).expect("writer");
        sz.set_content_methods(vec![EncoderMethod::COPY.into()]);
        sz.push_archive_entry(ArchiveEntry::new_file("big.bin"), Some(io::repeat(0).take(256 << 20))).expect("big");
        sz.push_archive_entry(ArchiveEntry::new_file("small.txt"), Some(&b"x"[..])).expect("small");
        sz.finish().expect("finish");
        let dest = root.path().join("out");
        fs::create_dir(&dest).expect("mkdir");
        let started = Instant::now();
        let (_, result) = extract(&archive, &[PathBuf::from("small.txt")], &dest, "", &CancelledNow);
        assert!(matches!(result, Err(OpError::Cancelled)));
        assert!(started.elapsed() < Duration::from_millis(300), "took {:?}", started.elapsed());
    }

    #[test]
    fn two_picks_with_one_name_both_land() {
        use zip::write::SimpleFileOptions;
        let root = tempfile::tempdir().expect("tmp");
        let zip_path = root.path().join("two.zip");
        let mut zip = zip::ZipWriter::new(File::create(&zip_path).expect("create"));
        zip.start_file("a/x.txt", SimpleFileOptions::default()).expect("a");
        zip.write_all(b"from a").expect("write");
        zip.start_file("b/x.txt", SimpleFileOptions::default()).expect("b");
        zip.write_all(b"from b").expect("write");
        zip.finish().expect("finish");
        let dest = root.path().join("out");
        fs::create_dir(&dest).expect("mkdir");
        extract(&zip_path, &[PathBuf::from("a/x.txt"), PathBuf::from("b/x.txt")], &dest, "", &Quietly)
            .1
            .expect("extract");
        assert_eq!(fs::read_to_string(dest.join("x.txt")).expect("first"), "from a");
        assert_eq!(fs::read_to_string(dest.join("x (copy).txt")).expect("second"), "from b");
    }

    #[test]
    fn a_password_zip_needs_the_right_password() {
        let root = tempfile::tempdir().expect("tmp");
        let zip = root.path().join("secret.zip");
        sample_zip(&zip, Some("hunter2"));
        assert_eq!(list(&zip).expect("names list without the password").len(), 3);
        let dest = root.path().join("out");
        fs::create_dir(&dest).expect("mkdir");
        let (_, result) = extract(&zip, &[], &dest, "", &Quietly);
        assert!(result.is_err_and(|e| e.to_string().ends_with("password needed")));
        let (_, result) = extract(&zip, &[], &dest, "wrong", &Quietly);
        assert!(result.is_err_and(|e| e.to_string().ends_with("wrong password")));
        extract(&zip, &[], &dest, "hunter2", &Quietly).1.expect("extract");
        assert_eq!(fs::read_to_string(dest.join("docs/a.txt")).expect("a"), "alpha");
    }

    #[test]
    fn compress_then_list_again_in_every_format() {
        let root = tempfile::tempdir().expect("tmp");
        let src = root.path().join("project");
        fs::create_dir_all(src.join("lib")).expect("mkdir");
        fs::write(src.join("readme.txt"), "hello").expect("write");
        fs::write(src.join("lib/code.rs"), "fn main() {}").expect("write");
        for (format, ext, password) in [
            (OutFormat::Zip, "zip", "pw"),
            (OutFormat::SevenZ, "7z", "pw"),
            (OutFormat::TarGz, "tar.gz", ""),
            (OutFormat::TarXz, "tar.xz", ""),
            (OutFormat::TarZst, "tar.zst", ""),
        ] {
            let archive = root.path().join(format!("project.{ext}"));
            let (undo, result) = compress(&[src.clone()], &archive, format, password, 0, &Quietly);
            result.unwrap_or_else(|e| panic!("{ext}: {e}"));
            assert!(matches!(&undo[..], [UndoStep::Created(p)] if *p == archive));
            let mut names: Vec<_> =
                list(&archive).unwrap_or_else(|e| panic!("{ext}: {e}")).into_iter().map(|e| e.path).collect();
            names.sort();
            assert_eq!(
                names,
                vec![
                    PathBuf::from("project"),
                    "project/lib".into(),
                    "project/lib/code.rs".into(),
                    "project/readme.txt".into()
                ],
                "{ext}"
            );
            let dest = root.path().join(format!("out-{ext}"));
            fs::create_dir(&dest).expect("mkdir");
            extract(&archive, &[], &dest, password, &Quietly).1.unwrap_or_else(|e| panic!("{ext}: {e}"));
            assert_eq!(fs::read_to_string(dest.join("project/lib/code.rs")).expect("code"), "fn main() {}", "{ext}");
        }
        assert_eq!(
            fs::read_dir(root.path())
                .expect("ls")
                .flatten()
                .filter(|e| e.file_name().as_bytes().starts_with(b"."))
                .count(),
            0,
            "no part file is left"
        );
    }

    #[test]
    fn split_volumes_extract_back_in_every_format() {
        let root = tempfile::tempdir().expect("tmp");
        let src = root.path().join("big");
        fs::create_dir(&src).expect("mkdir");
        // Incompressible bytes, so every format needs several volumes.
        let mut state = 0x2545_f491_4f6c_dd1du64;
        let data: Vec<u8> = (0..300_000)
            .map(|_| {
                state ^= state << 13;
                state ^= state >> 7;
                state ^= state << 17;
                state.to_le_bytes()[0]
            })
            .collect();
        fs::write(src.join("noise.bin"), &data).expect("write");
        for (format, ext, password) in
            [(OutFormat::Zip, "zip", "pw"), (OutFormat::SevenZ, "7z", "pw"), (OutFormat::TarZst, "tar.zst", "")]
        {
            let archive = root.path().join(format!("big.{ext}"));
            fs::write(volume_name(&archive, 1), "taken").expect("write");
            let (undo, result) = compress(&[src.clone()], &archive, format, password, 100_000, &Quietly);
            result.unwrap_or_else(|e| panic!("{ext}: {e}"));
            let taken = root.path().join(free_pick_name(archive.file_name().expect("name"), 1));
            let volumes: Vec<PathBuf> = (1..=undo.len()).map(|k| volume_name(&taken, k)).collect();
            assert!(undo.len() >= 3, "{ext}: {} volumes", undo.len());
            assert!(
                undo.iter().zip(&volumes).all(|(step, path)| matches!(step, UndoStep::Created(p) if p == path)),
                "{ext}"
            );
            assert!(
                volumes[..volumes.len() - 1].iter().all(|v| fs::metadata(v).expect("vol").len() == 100_000),
                "{ext}"
            );
            let dest = root.path().join(format!("out-{ext}"));
            fs::create_dir(&dest).expect("mkdir");
            extract(&volumes[0], &[], &dest, password, &Quietly).1.unwrap_or_else(|e| panic!("{ext}: {e}"));
            assert!(fs::read(dest.join("big/noise.bin")).expect("noise") == data, "{ext}");
        }
        assert_eq!(
            fs::read_dir(root.path())
                .expect("ls")
                .flatten()
                .filter(|e| e.file_name().as_bytes().starts_with(b"."))
                .count(),
            0,
            "no part file is left"
        );
    }
}
