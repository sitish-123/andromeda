// Microsoft Surface APO speaker EQ, stacked on the Dolby XML tuning.
// Ported from speaker-tuning-to-easyeffects lib/apo/surface.py (commit 2180c73):
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
//
// Only the MainEQ biquads are read; the compressor (VolumeDepMBDRC) and the
// resonance limiter (Crystal) are not reproduced.

use std::collections::HashSet;
use std::fs;
use std::path::{Path, PathBuf};

use anyhow::{Context, Result, anyhow, bail};
use serde_json::Value;

use crate::dax2::{addreg_sections, bound_installs, driver_version, fields, read_inf};
use crate::{Biquad, Tuning};

const INF_PREFIX: &str = "surfaceapoextension";
/// The endpoint property that names the APO's JSON config.
const CONFIG_PKEY: &str = "{c1f75c4c-3243-11ea-850d-2e728ce88125},0";
const IDENTITY: [f64; 5] = [1.0, 0.0, 0.0, 0.0, 0.0];

/// Adds the Surface APO speaker EQ that a package near the Dolby XML in `xml_dir` binds to this
/// codec; returns whether it found one.
pub fn stack(t: &mut Tuning, xml_dir: &Path, vendor_id: u32) -> Result<bool> {
    let Some(biquads) = find(xml_dir, vendor_id, &t.subsystem)? else { return Ok(false) };
    for e in t.endpoints.iter_mut().filter(|e| e.kind == "internal_speaker") {
        for p in e.profiles.values_mut() {
            p.biquads.clone_from(&biquads);
        }
    }
    Ok(true)
}

/// The Surface .inf files in the XML's folder, the folders under it, and the Surface packages
/// beside it (a driver store holds every package side by side).
fn candidate_infs(xml_dir: &Path) -> Vec<PathBuf> {
    let subdirs = |d: &Path| -> Vec<PathBuf> {
        fs::read_dir(d).into_iter().flatten().flatten().map(|e| e.path()).filter(|p| p.is_dir()).collect()
    };
    let name = |p: &Path| p.file_name().map(|n| n.to_string_lossy().to_ascii_lowercase()).unwrap_or_default();
    let mut dirs = vec![xml_dir.to_path_buf()];
    dirs.extend(subdirs(xml_dir));
    if let Some(parent) = xml_dir.parent() {
        dirs.extend(subdirs(parent).into_iter().filter(|d| {
            let n = name(d);
            n.starts_with(INF_PREFIX) || !n.contains(".inf_")
        }));
    }
    dirs.sort();
    dirs.dedup();
    dirs.iter()
        .flat_map(|d| subfiles(d))
        .filter(|f| {
            let n = name(f);
            n.starts_with(INF_PREFIX) && f.extension().is_some_and(|e| e.eq_ignore_ascii_case("inf"))
        })
        .collect()
}

fn subfiles(d: &Path) -> Vec<PathBuf> {
    fs::read_dir(d).into_iter().flatten().flatten().map(|e| e.path()).filter(|p| p.is_file()).collect()
}

/// The newest bound package's biquads; `None` when no Surface .inf binds this codec.
fn find(xml_dir: &Path, vendor_id: u32, subsystem: &str) -> Result<Option<Vec<Biquad>>> {
    let parts: HashSet<String> =
        [format!("dev_{:04x}", vendor_id & 0xffff), format!("subsys_{}", subsystem.to_lowercase())].into();
    let mut bound = Vec::new();
    for inf in candidate_infs(xml_dir) {
        let Ok(raw) = fs::read(&inf) else { continue };
        let s = read_inf(&raw);
        let config = bound_installs(&s, &parts).into_iter().find_map(|(_, install)| {
            addreg_sections(&s, &install).iter().flat_map(|a| s.get(a).into_iter().flatten()).find_map(|line| {
                let f = fields(line);
                (f.len() >= 5 && f[2].eq_ignore_ascii_case(CONFIG_PKEY))
                    .then(|| f[4].rsplit(['\\', '/']).next().unwrap_or_default().to_lowercase())
            })
        });
        if let Some(c) = config {
            bound.push((driver_version(&raw), inf, c));
        }
    }
    bound.sort_by(|a, b| (&a.0, &a.1).cmp(&(&b.0, &b.1)));
    let Some((_, inf, config)) = bound.pop() else { return Ok(None) };
    let dir = inf.parent().unwrap_or(Path::new("."));
    let path = subfiles(dir)
        .into_iter()
        .find(|f| f.file_name().is_some_and(|n| n.to_string_lossy().to_lowercase() == config))
        .with_context(|| format!("{} names {config}, which is not beside it", inf.display()))?;
    let text = fs::read_to_string(&path).with_context(|| format!("read {}", path.display()))?;
    let doc: Value = serde_json::from_str(text.trim_start_matches('\u{feff}'))
        .with_context(|| format!("{} is not JSON", path.display()))?;
    main_eq(&doc).with_context(|| format!("{}", path.display())).map(Some)
}

/// Every enabled `MainEQ` block of the 48 kHz `R/EFX` chain as per-channel biquads, identities dropped.
/// A block's coefficients interleave the channels section by section: L0, R0, L1, R1, ...
fn main_eq(doc: &Value) -> Result<Vec<Biquad>> {
    let store = doc["entities"]
        .as_array()
        .and_then(|e| e.iter().find(|e| e["name"] == "InitialValueStore"))
        .ok_or_else(|| anyhow!("no InitialValueStore"))?;
    let blocks = |node: &Value| -> Vec<(String, serde_json::Map<String, Value>)> {
        node["children"]
            .as_array()
            .into_iter()
            .flatten()
            .filter(|b| b["type"] == "complex")
            .map(|b| {
                let params = b["children"]
                    .as_array()
                    .into_iter()
                    .flatten()
                    .filter_map(|p| Some((p["name"].as_str()?.to_owned(), p["value"].clone())))
                    .collect();
                (b["name"].as_str().unwrap_or_default().to_owned(), params)
            })
            .collect()
    };
    let chain = store["children"]
        .as_array()
        .into_iter()
        .flatten()
        .filter(|n| n["name"] == "R/EFX")
        .map(blocks)
        .find(|bs| {
            bs.iter().any(|(_, p)| {
                p.get("InputFormats").and_then(Value::as_array).into_iter().flatten().any(|f| f["sample_rate"] == 48000)
            })
        })
        .ok_or_else(|| anyhow!("no 48 kHz R/EFX render chain"))?;
    let first = |v: &Value| v.as_array().and_then(|a| a.first()).cloned().unwrap_or_else(|| v.clone());
    let mut out = Vec::new();
    for (name, params) in &chain {
        let is_main_eq = name.strip_prefix("MainEQ").is_some_and(|n| n.bytes().all(|b| b.is_ascii_digit()));
        let enabled = params
            .get("Enabled")
            .map(first)
            .is_some_and(|e| e.as_bool().unwrap_or(false) || e.as_f64().is_some_and(|x| x != 0.0));
        if !is_main_eq || !enabled {
            continue;
        }
        let coeffs = params
            .get("Coefficients")
            .and_then(Value::as_array)
            .into_iter()
            .flatten()
            .map(|c| c.as_f64().filter(|x| x.is_finite()))
            .collect::<Option<Vec<f64>>>()
            .ok_or_else(|| anyhow!("{name} holds a coefficient that is not a finite number"))?;
        if coeffs.is_empty() || coeffs.len() % 10 != 0 {
            bail!("{name} holds {} coefficients, not a whole number of stereo biquad pairs", coeffs.len());
        }
        for (i, s) in coeffs.as_chunks::<5>().0.iter().enumerate() {
            if *s != IDENTITY {
                out.push(Biquad { speaker: u32::from(i % 2 == 1), coeffs: *s });
            }
        }
    }
    Ok(out)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::chain::{channel_eq, peaking_db};
    use crate::{Endpoint, Profile};

    /// RBJ peaking biquad at 48 kHz, normalised to a0 = 1.
    fn peaking(f0: f64, q: f64, gain_db: f64) -> [f64; 5] {
        let a = 10f64.powf(gain_db / 40.0);
        let w = 2.0 * std::f64::consts::PI * f0 / 48_000.0;
        let al = w.sin() / (2.0 * q);
        let a0 = 1.0 + al / a;
        [(1.0 + al * a) / a0, -2.0 * w.cos() / a0, (1.0 - al * a) / a0, -2.0 * w.cos() / a0, (1.0 - al / a) / a0]
    }

    #[test]
    fn surface_eq_beside_dolby_package_shapes_left_channel() -> Result<()> {
        let store = tempfile::tempdir()?;
        let dolby = store.path().join("dax3_ext_ms.inf_amd64_1");
        let pkg = store.path().join("surfaceapoextension.inf_amd64_2");
        fs::create_dir_all(&dolby)?;
        fs::create_dir_all(&pkg)?;
        fs::write(
            pkg.join("SurfaceApoExtension.inf"),
            "[Version]\nDriverVer=01/01/2024,1.0\n[Manufacturer]\nMS=Models,NTamd64\n\
             [Models.NTamd64]\nApo=Install,HDAUDIO\\FUNC_01&VEN_10EC&DEV_0294&SUBSYS_10431A63\n\
             [Install.NT]\nAddReg=Reg\n[Reg]\nHKR,FX\\0,\"{c1f75c4c-3243-11ea-850d-2e728ce88125},0\",,%13%\\SurfaceAPO_1.json\n",
        )?;
        let eq: Vec<f64> = [peaking(1000.0, 1.0, -6.0), IDENTITY].concat();
        let doc = serde_json::json!({"entities": [{"name": "InitialValueStore", "children": [
            {"name": "R/EFX", "children": [{"type": "complex", "name": "MainEQ", "children": [
                {"name": "Enabled", "value": [true]},
                {"name": "Coefficients", "value": peaking(300.0, 1.0, -12.0).repeat(2)},
                {"name": "InputFormats", "value": [{"sample_rate": 44100}]}]}]},
            {"name": "R/EFX", "children": [{"type": "complex", "name": "MainEQ", "children": [
                {"name": "Enabled", "value": [true]},
                {"name": "Coefficients", "value": eq},
                {"name": "InputFormats", "value": [{"sample_rate": 48000}]}]}]}]}]});
        fs::write(pkg.join("SURFACEAPO_1.JSON"), doc.to_string())?;
        let mut t = Tuning {
            source: "Dolby".into(),
            subsystem: "10431A63".into(),
            tuning_date: String::new(),
            band_freqs_hz: vec![100.0, 1000.0, 10000.0],
            endpoints: vec![Endpoint {
                kind: "internal_speaker".into(),
                channels: 2,
                profiles: [("music".into(), Profile::default())].into(),
            }],
        };
        assert!(stack(&mut t, &dolby, 0x10EC_0294)?);
        let p = &t.endpoints[0].profiles["music"];
        let at = |ch: usize, f: f64| {
            channel_eq(&t, p, ch).iter().map(|b| peaking_db(b.freq, b.q, b.gain_db, f)).sum::<f64>()
        };
        assert!((at(0, 1000.0) + 6.0).abs() < 1.0, "left at 1 kHz: {}", at(0, 1000.0));
        assert!(at(0, 300.0) > -3.0, "the 44.1 kHz chain is not read");
        assert!(at(1, 1000.0).abs() < 0.5, "right stays flat");
        Ok(())
    }
}
