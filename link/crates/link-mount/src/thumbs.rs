//! Thumbnails of a phone's photos and videos in the freedesktop thumbnail cache, so a file manager shows them without
//! reading each whole file over the network. One task asks the phone for them in turn, newest first; a file that
//! already has one is skipped. See <https://specifications.freedesktop.org/thumbnail-spec/latest/>.

use std::fmt::Write as _;
use std::io::Write as _;
use std::os::unix::fs::{DirBuilderExt, OpenOptionsExt};
use std::path::{Path, PathBuf};

use tokio::sync::mpsc;

use crate::link::Link;

/// The cache's `large` size.
const SIZE: u16 = 256;
/// Thumbnails asked for per folder listing.
pub const PER_LISTING: usize = 200;
const PICTURES: [&str; 14] =
    ["jpg", "jpeg", "png", "webp", "gif", "heic", "heif", "avif", "mp4", "mov", "mkv", "webm", "3gp", "m4v"];

pub struct Job {
    pub device: String,
    /// The browse path on the phone.
    pub path: String,
    /// The file under the mount, which the file manager names it by.
    pub file: PathBuf,
    pub mtime: u64,
}

pub fn is_picture(name: &str) -> bool {
    name.rsplit_once('.').is_some_and(|(_, extension)| PICTURES.contains(&extension.to_ascii_lowercase().as_str()))
}

/// Fills the cache from `jobs` until every sender is gone.
pub async fn run(link: Link, mut jobs: mpsc::Receiver<Job>) {
    let Some(dir) = cache_dir() else { return log::warn!("no cache folder for thumbnails") };
    if let Err(error) = std::fs::DirBuilder::new().recursive(true).mode(0o700).create(&dir) {
        return log::warn!("creating {}: {error}", dir.display());
    }
    while let Some(job) = jobs.recv().await {
        let uri = file_uri(&job.file);
        let target = dir.join(format!("{}.png", hex(&md5(uri.as_bytes()))));
        if target.exists() {
            continue;
        }
        match link.thumbnail(&job.device, &job.path, SIZE).await {
            Ok(png) => {
                if let Err(error) = save(&target, &png, &uri, job.mtime) {
                    log::info!("thumbnail of {}: {error}", job.path);
                }
            }
            Err(errno) => log::debug!("no thumbnail of {}: {errno:?}", job.path),
        }
    }
}

fn cache_dir() -> Option<PathBuf> {
    let cache = std::env::var_os("XDG_CACHE_HOME")
        .map(PathBuf::from)
        .or_else(|| std::env::var_os("HOME").map(|home| PathBuf::from(home).join(".cache")))?;
    Some(cache.join("thumbnails").join("large"))
}

/// Writes `png` with the `Thumb::URI` and `Thumb::MTime` the spec requires, whole or not at all.
fn save(target: &Path, png: &[u8], uri: &str, mtime: u64) -> std::io::Result<()> {
    let png = with_text(png, &[("Thumb::URI", uri), ("Thumb::MTime", &mtime.to_string())])
        .ok_or_else(|| std::io::Error::other("the phone's thumbnail is not a PNG"))?;
    let partial = target.with_extension("png.part");
    let mut file = std::fs::OpenOptions::new().write(true).create(true).truncate(true).mode(0o600).open(&partial)?;
    file.write_all(&png)?;
    std::fs::rename(partial, target)
}

/// `png` with a `tEXt` chunk per pair after its header; None when it is not a PNG.
fn with_text(png: &[u8], pairs: &[(&str, &str)]) -> Option<Vec<u8>> {
    const HEADER_END: usize = 8 + 4 + 4 + 13 + 4;
    let header_ok = png.starts_with(b"\x89PNG\r\n\x1a\n") && png.get(8..16) == Some(b"\0\0\0\x0dIHDR".as_slice());
    if !header_ok || png.len() < HEADER_END {
        return None;
    }
    let mut out = png[..HEADER_END].to_vec();
    for (key, value) in pairs {
        let data = [key.as_bytes(), b"\0", value.as_bytes()].concat();
        let body = [b"tEXt".as_slice(), &data].concat();
        out.extend_from_slice(&u32::try_from(data.len()).ok()?.to_be_bytes());
        out.extend_from_slice(&body);
        out.extend_from_slice(&crc32(&body).to_be_bytes());
    }
    out.extend_from_slice(&png[HEADER_END..]);
    Some(out)
}

/// `file://` and the path, escaped as `GLib`'s `g_filename_to_uri` escapes it, since the cache's file names are the MD5
/// of exactly that string.
fn file_uri(path: &Path) -> String {
    let mut uri = String::from("file://");
    for &byte in path.as_os_str().as_encoded_bytes() {
        if byte.is_ascii_alphanumeric() || b"-_.!~*'()/:@&=+$,".contains(&byte) {
            uri.push(char::from(byte));
        } else {
            let _ = write!(uri, "%{byte:02X}");
        }
    }
    uri
}

fn hex(bytes: &[u8]) -> String {
    bytes.iter().fold(String::new(), |mut out, byte| {
        let _ = write!(out, "{byte:02x}");
        out
    })
}

fn crc32(bytes: &[u8]) -> u32 {
    let mut crc = !0u32;
    for &byte in bytes {
        crc ^= u32::from(byte);
        for _ in 0..8 {
            crc = if crc & 1 == 1 { (crc >> 1) ^ 0xEDB8_8320 } else { crc >> 1 };
        }
    }
    !crc
}

/// RFC 1321. Only names cache files; nothing here relies on it resisting collisions.
#[expect(clippy::many_single_char_names, reason = "the names RFC 1321 uses")]
fn md5(input: &[u8]) -> [u8; 16] {
    const SHIFT: [u32; 16] = [7, 12, 17, 22, 5, 9, 14, 20, 4, 11, 16, 23, 6, 10, 15, 21];
    const K: [u32; 64] = [
        0xd76a_a478,
        0xe8c7_b756,
        0x2420_70db,
        0xc1bd_ceee,
        0xf57c_0faf,
        0x4787_c62a,
        0xa830_4613,
        0xfd46_9501,
        0x6980_98d8,
        0x8b44_f7af,
        0xffff_5bb1,
        0x895c_d7be,
        0x6b90_1122,
        0xfd98_7193,
        0xa679_438e,
        0x49b4_0821,
        0xf61e_2562,
        0xc040_b340,
        0x265e_5a51,
        0xe9b6_c7aa,
        0xd62f_105d,
        0x0244_1453,
        0xd8a1_e681,
        0xe7d3_fbc8,
        0x21e1_cde6,
        0xc337_07d6,
        0xf4d5_0d87,
        0x455a_14ed,
        0xa9e3_e905,
        0xfcef_a3f8,
        0x676f_02d9,
        0x8d2a_4c8a,
        0xfffa_3942,
        0x8771_f681,
        0x6d9d_6122,
        0xfde5_380c,
        0xa4be_ea44,
        0x4bde_cfa9,
        0xf6bb_4b60,
        0xbebf_bc70,
        0x289b_7ec6,
        0xeaa1_27fa,
        0xd4ef_3085,
        0x0488_1d05,
        0xd9d4_d039,
        0xe6db_99e5,
        0x1fa2_7cf8,
        0xc4ac_5665,
        0xf429_2244,
        0x432a_ff97,
        0xab94_23a7,
        0xfc93_a039,
        0x655b_59c3,
        0x8f0c_cc92,
        0xffef_f47d,
        0x8584_5dd1,
        0x6fa8_7e4f,
        0xfe2c_e6e0,
        0xa301_4314,
        0x4e08_11a1,
        0xf753_7e82,
        0xbd3a_f235,
        0x2ad7_d2bb,
        0xeb86_d391,
    ];
    let mut message = input.to_vec();
    message.push(0x80);
    while message.len() % 64 != 56 {
        message.push(0);
    }
    message.extend_from_slice(&(input.len() as u64).wrapping_mul(8).to_le_bytes());
    let mut state = [0x6745_2301u32, 0xefcd_ab89, 0x98ba_dcfe, 0x1032_5476];
    for block in message.as_chunks::<64>().0 {
        let words: Vec<u32> = block.as_chunks::<4>().0.iter().map(|word| u32::from_le_bytes(*word)).collect();
        let [mut a, mut b, mut c, mut d] = state;
        for i in 0..64 {
            let (f, g) = match i / 16 {
                0 => ((b & c) | (!b & d), i),
                1 => ((d & b) | (!d & c), (5 * i + 1) % 16),
                2 => (b ^ c ^ d, (3 * i + 5) % 16),
                _ => (c ^ (b | !d), (7 * i) % 16),
            };
            let sum = f.wrapping_add(a).wrapping_add(K[i]).wrapping_add(words[g]);
            (a, d, c) = (d, c, b);
            b = b.wrapping_add(sum.rotate_left(SHIFT[(i / 16) * 4 + i % 4]));
        }
        for (word, add) in state.iter_mut().zip([a, b, c, d]) {
            *word = word.wrapping_add(add);
        }
    }
    let mut digest = [0; 16];
    for (out, word) in digest.as_chunks_mut::<4>().0.iter_mut().zip(state) {
        out.copy_from_slice(&word.to_le_bytes());
    }
    digest
}
