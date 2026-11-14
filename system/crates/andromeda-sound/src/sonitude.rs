// Sonitude (HP Bang & Olufsen) speaker presets.
// Ported from speaker-tuning-to-easyeffects lib/apo/sonitude.py (commit 2180c73):
// Copyright (c) Antoine Cellerier, MIT licence:
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions: The above copyright
// notice and this permission notice shall be included in all copies or
// substantial portions of the Software. THE SOFTWARE IS PROVIDED "AS IS",
// WITHOUT WARRANTY OF ANY KIND.

use std::collections::{BTreeMap, HashMap};
use std::fs::File;
use std::io::Read;
use std::path::Path;

use anyhow::{Context, Result, anyhow, bail};
use zip::ZipArchive;

use crate::chain::DAX;
use crate::{Endpoint, PeqFilter, Profile, Tuning};

pub const CURVE_POINTS: usize = 265;
const CURVE_MARKER: [u8; 2] = 265u16.to_le_bytes();
const MAX_CURVE_DB: f32 = 120.0;
const SECTION_TABLE: usize = 0x28;
const MAX_KEYS: i32 = 4096;
const HIGH_PASS_KEYS: [i32; 2] = [19, 20];
/// A rise between grid neighbours above this is the step at the high-pass corner.
const BREAK_STEP_DB: f32 = 6.0;

/// Content mode folder per style: `AnyContent` is the archive's "Music".
pub const PROFILES: [(&str, &str); 3] = [("music", "AnyContent"), ("movie", "Movie"), ("voice", "Voice")];

/// Curve point k sits at 10 * 2^(k/24) Hz.
fn grid_hz(k: usize) -> f64 {
    10.0 * 2f64.powf(f64::from(u16::try_from(k).unwrap_or(u16::MAX)) / 24.0)
}

pub fn curve_grid_hz() -> Vec<f32> {
    #[allow(clippy::cast_possible_truncation)]
    (0..CURVE_POINTS).map(|k| grid_hz(k) as f32).collect()
}

/// The SEAPODAT archive name for codec `vendor_id` (`0x10EC0285`) on `bus` (`HDAUDIO`, `INTELAUDIO`), upper case.
pub fn archive_name(bus: &str, vendor_id: u32) -> String {
    format!("SEAPODAT.{bus}.FUNC_01-VEN_{:04X}-DEV_{:04X}.ZIP", vendor_id >> 16, vendor_id & 0xffff)
}

fn u32_at(d: &[u8], o: usize) -> Option<u32> {
    Some(u32::from_le_bytes(d.get(o..o + 4)?.try_into().ok()?))
}

fn i32_at(d: &[u8], o: usize) -> Option<i32> {
    Some(i32::from_le_bytes(d.get(o..o + 4)?.try_into().ok()?))
}

/// Subsystem ID ("103C860F") to preset number, from SSIDMAP.bin.
pub fn read_ssidmap(d: &[u8]) -> Result<HashMap<String, u32>, String> {
    let bad = || "SSIDMAP.bin has an unknown layout".to_string();
    if d.len() < 16 || &d[..4] != b"SRSM" {
        return Err(bad());
    }
    let count = u32_at(d, 8).ok_or_else(bad)? as usize;
    let size = u32_at(d, 12).ok_or_else(bad)? as usize;
    if !(size == 20 || size == 32) || count.checked_mul(size).and_then(|n| n.checked_add(16)) != Some(d.len()) {
        return Err(bad());
    }
    let mut out = HashMap::new();
    for rec in d[16..].chunks_exact(size) {
        let number = u32_at(rec, 0).ok_or_else(bad)?;
        let id = &rec[4..];
        if id.len() >= 15 && id.starts_with(b"SUBSYS_") && id[7..15].iter().all(u8::is_ascii_hexdigit) {
            let key = String::from_utf8_lossy(&id[7..15]).to_ascii_uppercase();
            out.entry(key).or_insert(number);
        }
    }
    Ok(out)
}

/// The first two 265-point curves: u32 length (532 or 534 bytes), u16 265, 265 f32 LE.
pub fn read_curves(d: &[u8]) -> Vec<Vec<f32>> {
    let want = 2 + 4 * CURVE_POINTS;
    let mut out = Vec::new();
    let mut i = 4;
    while i + 2 <= d.len() && out.len() < 2 {
        if d[i..i + 2] == CURVE_MARKER {
            let o = i - 4;
            let len = u32_at(d, o).unwrap_or(0) as usize;
            if (len == want || len == want + 2) && o + 4 + len <= d.len() {
                out.push(
                    d[o + 6..o + 6 + 4 * CURVE_POINTS]
                        .as_chunks::<4>()
                        .0
                        .iter()
                        .map(|&b| f32::from_le_bytes(b))
                        .collect(),
                );
            }
        }
        i += 1;
    }
    out
}

/// Preset.bin's keyed values by (stage, key); empty when the walk does not end at EOF.
fn stage_values(d: &[u8]) -> HashMap<(i32, i32), &[u8]> {
    walk_stages(d).unwrap_or_default()
}

fn walk_stages(d: &[u8]) -> Option<HashMap<(i32, i32), &[u8]>> {
    if d.len() < SECTION_TABLE + 32 {
        return None;
    }
    let mut o = u32_at(d, SECTION_TABLE + 16)? as usize;
    let mut out = HashMap::new();
    while o < d.len() {
        let (stage, first, last) = (i32_at(d, o)?, i32_at(d, o + 4)?, i32_at(d, o + 8)?);
        o += 12;
        if first > last {
            continue;
        }
        if last.checked_sub(first)? >= MAX_KEYS {
            return None;
        }
        for key in first..=last {
            let n = u32_at(d, o)? as usize;
            o += 4;
            let v = d.get(o..o.checked_add(n)?)?;
            if n > 0 {
                out.insert((stage, key), v);
            }
            o += n;
        }
    }
    (o == d.len()).then_some(out)
}

/// One channel's high-pass field.
#[derive(Debug, Clone, Copy, PartialEq)]
pub enum HighPass {
    Off,
    Unread,
    Corner(f32),
}

fn high_pass(values: &HashMap<(i32, i32), &[u8]>, key: i32) -> Result<HighPass, String> {
    let Some(p) = values.get(&(5, key)).filter(|p| p.len() == 5) else {
        return Ok(HighPass::Unread);
    };
    if p[0] & 1 == 0 {
        return Ok(HighPass::Off);
    }
    let hz = f32::from_le_bytes([p[1], p[2], p[3], p[4]]);
    if !(hz.is_finite() && (10.0..=1000.0).contains(&hz)) {
        return Err(format!("a high-pass at {hz} Hz"));
    }
    Ok(HighPass::Corner(hz))
}

/// Points below the corner and a step up just above it are the stored high-pass segment:
/// they hold the in-band level, and two Butterworth biquads at the corner cut below it.
fn hold_above_corner(curve: &[f32], hp: HighPass) -> Vec<f32> {
    let HighPass::Corner(corner) = hp else {
        return curve.to_vec();
    };
    let hz = curve_grid_hz();
    let edge = corner * (1.0 + 1e-6);
    let mut n = hz.iter().filter(|&&f| f <= edge).count();
    let step = (n.saturating_sub(1)..curve.len() - 1).find(|&i| curve[i + 1] - curve[i] > BREAK_STEP_DB);
    if let Some(k) = step
        && k >= n
        && hz[k] <= edge * 2f32.powf(1.0 / 24.0)
    {
        n = k + 1;
    }
    let level = curve.get(n).copied().unwrap_or(0.0);
    (0..curve.len()).map(|i| if i < n { level } else { curve[i] }).collect()
}

/// One speaker preset: left and right dB cuts on `curve_grid_hz`, and each channel's high-pass.
#[derive(Debug, Clone, PartialEq)]
pub struct Preset {
    pub curves: [Vec<f32>; 2],
    pub high_pass: [HighPass; 2],
}

/// Parse one Preset.bin into its speaker curves and high-pass corners.
pub fn read_preset(d: &[u8]) -> Result<Preset, String> {
    let curves = read_curves(d);
    if curves.is_empty() {
        return Err("the speaker preset holds no EQ curve".into());
    }
    if !curves.iter().flatten().all(|v| v.is_finite() && v.abs() <= MAX_CURVE_DB) {
        return Err("the speaker preset's EQ curve holds values no tuning uses".into());
    }
    let values = stage_values(d);
    let hp = [high_pass(&values, HIGH_PASS_KEYS[0])?, high_pass(&values, HIGH_PASS_KEYS[1])?];
    // A preset with one curve uses it, and its high-pass, for both channels.
    let right = usize::from(curves.len() > 1);
    let high_pass = [hp[0], hp[right]];
    // Cuts only: on two measured laptops Windows applies none of the curve's boosts.
    let shape = |i: usize| hold_above_corner(&curves[i], high_pass[i]).into_iter().map(|v| v.min(0.0)).collect();
    Ok(Preset { curves: [shape(0), shape(right)], high_pass })
}

/// Grid points per 1/3 octave; the tuning keeps one band per third from 40 Hz.
const THIRD: usize = 8;
const FIRST_BAND: usize = 48;

/// The 1/3-octave band centres a Sonitude tuning stores.
pub fn band_centres() -> Vec<f64> {
    (FIRST_BAND..CURVE_POINTS).step_by(THIRD).map(grid_hz).collect()
}

/// A curve as the mean over each 1/3-octave band, so a narrow cut between centres still counts.
fn to_bands(curve: &[f32]) -> Vec<f64> {
    (FIRST_BAND..CURVE_POINTS)
        .step_by(THIRD)
        .map(|k| {
            let w = &curve[k - THIRD / 2..(k + THIRD / 2).min(CURVE_POINTS)];
            w.iter().map(|&v| f64::from(v)).sum::<f64>() / f64::from(u8::try_from(w.len()).unwrap_or(1))
        })
        .collect()
}

/// Butterworth-4 as two cascaded 2nd-order high-pass sections.
const BUTTERWORTH_4_Q: [f64; 2] = [0.541_196_1, 1.306_563];

/// One style's profile: the curves as a per-channel audio-optimizer curve, the high-pass as speaker filters.
pub fn profile(p: &Preset) -> Profile {
    let mut cp = BTreeMap::new();
    cp.insert("audio-optimizer-enable".to_owned(), vec![1.0]);
    for (ch, c) in p.curves.iter().enumerate() {
        cp.insert(format!("audio-optimizer-bands.ch_{ch:02}"), to_bands(c).iter().map(|v| v * DAX).collect());
    }
    let speaker_peq = (0..2u32)
        .filter_map(|ch| match p.high_pass[ch as usize] {
            HighPass::Corner(hz) => Some(BUTTERWORTH_4_Q.map(|q| PeqFilter {
                speaker: ch,
                kind: 4,
                freq_hz: f64::from(hz),
                gain_db: 0.0,
                q: Some(q),
                slope: None,
            })),
            _ => None,
        })
        .flatten()
        .collect();
    Profile { cp, speaker_peq, ..Profile::default() }
}

/// Members over this are refused unread: archives are rehostable downloads, and the real ones stay under 8 KiB.
const MAX_MEMBER_BYTES: u64 = 1 << 20;
const MAX_MEMBERS: usize = 10_000;

/// The tuning archive `path` holds for codec subsystem `subsystem`; `None` when its map does not name it.
pub fn read_archive(path: &Path, subsystem: &str) -> Result<Option<Tuning>> {
    let mut zip = ZipArchive::new(File::open(path).with_context(|| format!("open {}", path.display()))?)
        .with_context(|| format!("read {}", path.display()))?;
    if zip.len() > MAX_MEMBERS {
        bail!("{} holds more members than a tuning archive", path.display());
    }
    let names: HashMap<String, String> = zip
        .file_names()
        .filter_map(Result::ok)
        .map(|n| (n.replace('\\', "/").to_ascii_lowercase(), n.into_owned()))
        .collect();
    let mut member = |name: &str| -> Result<Option<Vec<u8>>> {
        let Some(real) = names.get(name) else { return Ok(None) };
        let mut f = zip.by_name(real)?;
        if f.size() > MAX_MEMBER_BYTES {
            bail!("{name} is larger than a tuning file");
        }
        let mut b = Vec::new();
        f.by_ref().take(MAX_MEMBER_BYTES).read_to_end(&mut b)?;
        Ok(Some(b))
    };
    let map = member("ssidmap.bin")?.context("the archive has no SSIDMAP.bin")?;
    let Some(&number) = read_ssidmap(&map).map_err(anyhow::Error::msg)?.get(&subsystem.to_ascii_uppercase()) else {
        return Ok(None);
    };
    let mut profiles = BTreeMap::new();
    for (style, folder) in PROFILES {
        let name = format!("{number}/0/efx/anyoutput/{}/preset.bin", folder.to_ascii_lowercase());
        if let Some(d) = member(&name)? {
            let preset = read_preset(&d).map_err(|e| anyhow!("preset {number} ({style}): {e}"))?;
            profiles.insert(style.to_owned(), profile(&preset));
        }
    }
    if !profiles.contains_key("music") {
        bail!("preset {number} has no speaker file");
    }
    Ok(Some(Tuning {
        source: "Sonitude".to_owned(),
        subsystem: subsystem.to_ascii_uppercase(),
        tuning_date: String::new(),
        band_freqs_hz: band_centres(),
        endpoints: vec![Endpoint { kind: "internal_speaker".to_owned(), channels: 2, profiles }],
    }))
}

#[cfg(test)]
mod tests {
    use std::io::Write;

    use zip::write::SimpleFileOptions;

    use super::*;

    fn curve_value(points: &[f32]) -> Vec<u8> {
        let mut v = 265u16.to_le_bytes().to_vec();
        for p in points {
            v.extend(p.to_le_bytes());
        }
        v.extend([0, 0]);
        v
    }

    /// A Preset.bin: header pointing at one stage-5 block with keys 13..=20.
    fn preset_bin(left: &[f32], right: &[f32], corner: Option<f32>) -> Vec<u8> {
        let mut d = vec![0u8; 72];
        d[0x38..0x3c].copy_from_slice(&72u32.to_le_bytes());
        for n in [5i32, 13, 20] {
            d.extend(n.to_le_bytes());
        }
        let mut hp = vec![u8::from(corner.is_some())];
        hp.extend(corner.unwrap_or(0.0).to_le_bytes());
        let off = [0u8; 5].to_vec();
        for value in [curve_value(left), curve_value(right), vec![], vec![], vec![], vec![], hp, off] {
            d.extend(u32::try_from(value.len()).unwrap_or_default().to_le_bytes());
            d.extend(value);
        }
        d
    }

    fn ssidmap(entries: &[(u32, &str)]) -> Vec<u8> {
        let mut d = b"SRSM".to_vec();
        for n in [1, u32::try_from(entries.len()).unwrap_or_default(), 20] {
            d.extend(n.to_le_bytes());
        }
        for (number, id) in entries {
            d.extend(number.to_le_bytes());
            let mut name = format!("SUBSYS_{id}").into_bytes();
            name.resize(16, 0);
            d.extend(name);
        }
        d
    }

    #[test]
    fn archive_preset_gives_cut_curves_and_high_pass() -> Result<()> {
        let mut left = vec![0.0f32; CURVE_POINTS];
        (0..80).for_each(|k| left[k] = -40.0); // the stored segment below the 100 Hz corner
        (136..=152).for_each(|k| left[k] = -6.0);
        left[200] = 3.0;
        let right = vec![-3.0f32; CURVE_POINTS];
        let dir = tempfile::tempdir()?;
        let path = dir.path().join(archive_name("HDAUDIO", 0x10EC_0285));
        let mut zip = zip::ZipWriter::new(File::create(&path)?);
        zip.start_file("SSIDMAP.bin", SimpleFileOptions::default())?;
        zip.write_all(&ssidmap(&[(7, "103C860F")]))?;
        zip.start_file("7\\0\\EFX\\AnyOutput\\AnyContent\\Preset.bin", SimpleFileOptions::default())?;
        zip.write_all(&preset_bin(&left, &right, Some(100.0)))?;
        zip.finish()?;

        assert_eq!(read_archive(&path, "103C0000")?, None);
        let t = read_archive(&path, "103c860f")?.context("subsystem not found")?;
        assert_eq!((t.source.as_str(), t.band_freqs_hz.len()), ("Sonitude", 28));
        let music = &t.endpoints[0].profiles["music"];
        let db = |ch: &str, i: usize| music.cp[&format!("audio-optimizer-bands.{ch}")][i] / DAX;
        // Band 0 is 40 Hz, below the corner: held at the in-band level, the biquads cut there.
        assert!(db("ch_00", 0).abs() < 1e-6);
        assert!((db("ch_00", 12) + 6.0).abs() < 1e-6, "512 Hz band keeps the cut");
        assert!(db("ch_00", 19).abs() < 1e-6, "boost at 3.2 kHz is dropped");
        assert!((0..28).all(|i| (db("ch_01", i) + 3.0).abs() < 1e-6));
        let hp: Vec<_> = music.speaker_peq.iter().map(|f| (f.speaker, f.kind, f.freq_hz)).collect();
        assert_eq!(hp, vec![(0, 4, 100.0), (0, 4, 100.0)]);
        Ok(())
    }
}
