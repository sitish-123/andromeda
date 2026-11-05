//! Just enough PNG to keep freedesktop thumbnail metadata: read and add `tEXt` chunks, and write the 1x1 fail
//! marker. Thumbnail bytes come from thumbnailers that parsed untrusted files, so every length is checked.

const SIGNATURE: &[u8; 8] = b"\x89PNG\r\n\x1a\n";

fn chunk(kind: [u8; 4], data: &[u8]) -> Vec<u8> {
    let len = u32::try_from(data.len()).expect("chunk under 4 GiB");
    let mut out = Vec::with_capacity(data.len() + 12);
    out.extend_from_slice(&len.to_be_bytes());
    out.extend_from_slice(&kind);
    out.extend_from_slice(data);
    let mut crc = crc32fast::Hasher::new();
    crc.update(&kind);
    crc.update(data);
    out.extend_from_slice(&crc.finalize().to_be_bytes());
    out
}

fn text_chunk(key: &str, value: &str) -> Vec<u8> {
    let mut data = Vec::with_capacity(key.len() + value.len() + 1);
    data.extend_from_slice(key.as_bytes());
    data.push(0);
    data.extend_from_slice(value.as_bytes());
    chunk(*b"tEXt", &data)
}

/// Chunks as (type, data) slices; `None` for a file that is not a well-formed PNG up to IEND.
fn chunks(png: &[u8]) -> Option<Vec<([u8; 4], &[u8])>> {
    let mut rest = png.strip_prefix(SIGNATURE.as_slice())?;
    let mut out = Vec::new();
    loop {
        let len = usize::try_from(u32::from_be_bytes(rest.get(..4)?.try_into().ok()?)).ok()?;
        let kind: [u8; 4] = rest.get(4..8)?.try_into().ok()?;
        let end = 8usize.checked_add(len)?;
        let data = rest.get(8..end)?;
        let crc = u32::from_be_bytes(rest.get(end..end.checked_add(4)?)?.try_into().ok()?);
        let mut hasher = crc32fast::Hasher::new();
        hasher.update(&kind);
        hasher.update(data);
        if hasher.finalize() != crc {
            return None;
        }
        rest = rest.get(end.checked_add(4)?..)?;
        out.push((kind, data));
        if &kind == b"IEND" {
            return Some(out);
        }
    }
}

/// `tEXt` key/value pairs of a PNG; empty for a malformed file.
#[must_use]
pub fn read_text(png: &[u8]) -> Vec<(String, String)> {
    chunks(png)
        .unwrap_or_default()
        .into_iter()
        .filter(|(kind, _)| kind == b"tEXt")
        .filter_map(|(_, data)| {
            let nul = data.iter().position(|&b| b == 0)?;
            Some((String::from_utf8_lossy(&data[..nul]).into(), String::from_utf8_lossy(&data[nul + 1..]).into()))
        })
        .collect()
}

#[must_use]
pub fn text_value(png: &[u8], key: &str) -> Option<String> {
    read_text(png).into_iter().find(|(k, _)| k == key).map(|(_, v)| v)
}

/// The same PNG with `pairs` as `tEXt` chunks after IHDR, replacing chunks with the same keys. `None` when the
/// input is not a PNG.
#[must_use]
pub fn with_text(png: &[u8], pairs: &[(&str, &str)]) -> Option<Vec<u8>> {
    let parsed = chunks(png)?;
    if parsed.first().map(|(kind, _)| kind) != Some(b"IHDR") {
        return None;
    }
    let mut out = SIGNATURE.to_vec();
    for (i, (kind, data)) in parsed.iter().enumerate() {
        let replaced = kind == b"tEXt"
            && pairs.iter().any(|(k, _)| data.strip_prefix(k.as_bytes()).is_some_and(|r| r.first() == Some(&0)));
        if !replaced {
            out.extend_from_slice(&chunk(*kind, data));
        }
        if i == 0 {
            for (k, v) in pairs {
                out.extend_from_slice(&text_chunk(k, v));
            }
        }
    }
    Some(out)
}

/// Width and height from IHDR.
#[must_use]
pub fn size(png: &[u8]) -> Option<(u32, u32)> {
    let parsed = chunks(png)?;
    let (kind, data) = parsed.first()?;
    if kind != b"IHDR" || data.len() < 8 {
        return None;
    }
    Some((u32::from_be_bytes(data[0..4].try_into().ok()?), u32::from_be_bytes(data[4..8].try_into().ok()?)))
}

/// A 1x1 transparent PNG carrying `pairs`: the freedesktop fail marker.
#[must_use]
pub fn marker(pairs: &[(&str, &str)]) -> Vec<u8> {
    // IHDR: 1x1, 8-bit RGBA. IDAT: zlib stream with one stored block holding filter byte 0 and 4 zero bytes.
    let ihdr = [0, 0, 0, 1, 0, 0, 0, 1, 8, 6, 0, 0, 0];
    let raw = [0u8; 5];
    let mut idat = vec![0x78, 0x01, 0x01, 5, 0, 0xfa, 0xff];
    idat.extend_from_slice(&raw);
    let (mut a, mut b) = (1u32, 0u32);
    for byte in raw {
        a = (a + u32::from(byte)) % 65521;
        b = (b + a) % 65521;
    }
    idat.extend_from_slice(&((b << 16) | a).to_be_bytes());
    let mut out = SIGNATURE.to_vec();
    out.extend_from_slice(&chunk(*b"IHDR", &ihdr));
    for (k, v) in pairs {
        out.extend_from_slice(&text_chunk(k, v));
    }
    out.extend_from_slice(&chunk(*b"IDAT", &idat));
    out.extend_from_slice(&chunk(*b"IEND", &[]));
    out
}
