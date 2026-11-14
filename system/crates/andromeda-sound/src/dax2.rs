// Dolby DAX2 tunings that a driver .inf writes as endpoint registry values.
// Ported from speaker-tuning-to-easyeffects lib/apo/dax2.py and lib/apo/inf.py (commit 2180c73):
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

use std::collections::{BTreeMap, HashMap, HashSet};

use anyhow::{Result, anyhow, bail};

use crate::{Endpoint, Profile, Tuning};

/// Dolby's endpoint property set; each value is `{guid},<index>`.
const EP_PKEY: &str = "{f112024a-fe30-42a8-80ab-8dd825a06f78}";
/// Indices from here on are DAX3's registry form, not DAX2.
const DAX3_FIRST_INDEX: u32 = 48;
const VT_I4: u16 = 0x0003;
const VT_BLOB: u16 = 0x0041;
const AUDIO_OPTIMIZER: u32 = 18;
/// Single values the chain reads, as the DAX3 fields they stand for (1/16 dB units).
const NUMBERS: [(u32, &str); 4] = [
    (6, "volume-leveler-in-target"),
    (7, "volume-leveler-out-target"),
    (8, "volume-leveler-amount"),
    (9, "volume-leveler-enable"),
];

/// Whether .inf bytes carry the DAX2 key, in ASCII or UTF-16.
pub fn mentions_dax2(raw: &[u8]) -> bool {
    let key = EP_PKEY.as_bytes();
    let wide: Vec<u8> = key.iter().flat_map(|&b| [b, 0]).collect();
    let lower = raw.to_ascii_lowercase();
    lower.windows(key.len()).any(|w| w == key) || lower.windows(wide.len()).any(|w| w == wide)
}

/// An .inf as lower-cased section name to its lines: comments gone, `\` joins lines, `%TOKEN%` substituted.
pub fn read_inf(raw: &[u8]) -> HashMap<String, Vec<String>> {
    let text = match raw {
        [0xff, 0xfe, rest @ ..] => String::from_utf16_lossy(
            &rest.as_chunks::<2>().0.iter().map(|&c| u16::from_le_bytes(c)).collect::<Vec<_>>(),
        ),
        [0xfe, 0xff, rest @ ..] => String::from_utf16_lossy(
            &rest.as_chunks::<2>().0.iter().map(|&c| u16::from_be_bytes(c)).collect::<Vec<_>>(),
        ),
        _ => String::from_utf8_lossy(raw.strip_prefix(b"\xef\xbb\xbf").unwrap_or(raw)).into_owned(),
    };
    let mut sections: HashMap<String, Vec<String>> = HashMap::new();
    let mut current: Option<String> = None;
    let mut pending = String::new();
    for line in text.lines() {
        let line = pending.clone() + strip_comment(line).trim();
        if let Some(head) = line.strip_suffix('\\') {
            head.clone_into(&mut pending);
            continue;
        }
        pending.clear();
        if line.is_empty() {
            continue;
        }
        if line.starts_with('[') && line.ends_with(']') {
            let name = line[1..line.len() - 1].trim().to_lowercase();
            sections.entry(name.clone()).or_default();
            current = Some(name);
        } else if let Some(c) = &current {
            sections.entry(c.clone()).or_default().push(line);
        }
    }
    let strings: HashMap<String, String> = sections
        .get("strings")
        .into_iter()
        .flatten()
        .filter_map(|l| l.split_once('='))
        .map(|(k, v)| (k.trim().to_lowercase(), v.trim().trim_matches('"').to_owned()))
        .collect();
    let subst = |line: &str| {
        let mut out = String::new();
        let mut rest = line;
        while let Some(a) = rest.find('%') {
            let Some(b) = rest[a + 1..].find('%') else { break };
            out.push_str(&rest[..a]);
            let token = &rest[a + 1..a + 1 + b];
            match strings.get(&token.to_lowercase()) {
                Some(v) if v.contains(',') => {
                    out.push('"');
                    out.push_str(v);
                    out.push('"');
                }
                Some(v) => out.push_str(v),
                None => out.push_str(&rest[a..=a + 1 + b]),
            }
            rest = &rest[a + b + 2..];
        }
        out + rest
    };
    sections.into_iter().map(|(name, lines)| (name, lines.iter().map(|l| subst(l)).collect())).collect()
}

/// The line up to its first `;` outside double quotes.
fn strip_comment(line: &str) -> &str {
    let mut quoted = false;
    for (i, c) in line.char_indices() {
        match c {
            '"' => quoted = !quoted,
            ';' if !quoted => return &line[..i],
            _ => {}
        }
    }
    line
}

/// Comma-separated fields, unquoted; a quoted comma stays inside.
pub(crate) fn fields(value: &str) -> Vec<String> {
    let (mut out, mut cur, mut quoted) = (Vec::new(), String::new(), false);
    for c in value.chars() {
        match c {
            '"' => quoted = !quoted,
            ',' if !quoted => out.push(std::mem::take(&mut cur).trim().to_owned()),
            _ => cur.push(c),
        }
    }
    out.push(cur.trim().to_owned());
    out
}

pub(crate) type Sections = HashMap<String, Vec<String>>;

/// (hardware ID, install section) for each model entry whose hardware ID carries all `parts`.
/// Walks [Manufacturer] to the model sections Windows reads on x64.
pub(crate) fn bound_installs(s: &Sections, parts: &HashSet<String>) -> Vec<(String, String)> {
    let mut out = Vec::new();
    for line in s.get("manufacturer").into_iter().flatten() {
        let Some((_, value)) = line.split_once('=') else { continue };
        let f = fields(value);
        let (base, decorations) = (&f[0], &f[1..]);
        let pick = |want: &dyn Fn(&str) -> bool| -> Vec<String> {
            decorations.iter().filter(|d| want(&d.to_lowercase())).map(|d| format!("{base}.{d}")).collect()
        };
        let mut models = pick(&|d| d.starts_with("ntamd64"));
        if models.is_empty() {
            models = pick(&|d| d == "nt");
        }
        if models.is_empty() {
            models = vec![base.clone()];
        }
        for model in models {
            for entry in s.get(&model.to_lowercase()).into_iter().flatten() {
                let Some((_, rhs)) = entry.split_once('=') else { continue };
                let f = fields(rhs);
                let hwid = f[1..].iter().find(|h| {
                    let ids: HashSet<String> = h.to_lowercase().split(['\\', '&']).map(str::to_owned).collect();
                    parts.is_subset(&ids)
                });
                if let Some(h) = hwid {
                    out.push((h.clone(), f[0].clone()));
                }
            }
        }
    }
    out
}

/// Every value a section's `key` lines list.
fn directive(s: &Sections, section: &str, key: &str) -> Vec<String> {
    s.get(section)
        .into_iter()
        .flatten()
        .filter_map(|l| l.split_once('='))
        .filter(|(k, _)| k.trim().eq_ignore_ascii_case(key))
        .flat_map(|(_, v)| fields(v))
        .filter(|v| !v.is_empty())
        .collect()
}

/// The `AddReg` sections of the x64 install section and of the interfaces it adds.
pub(crate) fn addreg_sections(s: &Sections, install: &str) -> Vec<String> {
    let Some(section) =
        [".ntamd64", ".nt", ""].iter().map(|x| format!("{install}{x}").to_lowercase()).find(|n| s.contains_key(n))
    else {
        return Vec::new();
    };
    let interfaces = s
        .get(&format!("{section}.interfaces"))
        .into_iter()
        .flatten()
        .filter_map(|l| l.split_once('='))
        .filter(|(k, _)| k.trim().eq_ignore_ascii_case("addinterface"))
        .filter_map(|(_, v)| fields(v).get(2).map(|t| t.to_lowercase()));
    let mut names = Vec::new();
    for target in std::iter::once(section.clone()).chain(interfaces) {
        for n in directive(s, &target, "addreg") {
            let n = n.to_lowercase();
            if !names.contains(&n) {
                names.push(n);
            }
        }
    }
    names
}

/// Preset name to index to the HKR line's data fields, for each preset an
/// `OEMSettingsOverride` line names.
fn presets(lines: &[&String]) -> BTreeMap<String, BTreeMap<u32, Vec<String>>> {
    let mut named = HashSet::new();
    for line in lines {
        let f: Vec<String> = fields(line).iter().map(|x| x.to_lowercase()).collect();
        if let Some(i) = f.iter().position(|x| x == "oemsettingsoverride") {
            named.extend(f[i + 1..].iter().filter(|x| !x.is_empty()).cloned());
        }
    }
    let mut out: BTreeMap<String, BTreeMap<u32, Vec<String>>> = BTreeMap::new();
    for line in lines {
        let f = fields(line);
        if f.len() < 5 || !f[2].to_lowercase().starts_with(&format!("{EP_PKEY},")) {
            continue;
        }
        let Some(name) = f[1].split('\\').nth(1).filter(|n| named.contains(&n.to_lowercase())) else { continue };
        let Some(Ok(index)) = f[2].rsplit(',').next().map(|i| i.trim().parse::<u32>()) else { continue };
        out.entry(name.to_owned()).or_default().insert(index, f[4..].to_vec());
    }
    out
}

/// The speaker's preset: the only one, or the one named for the speaker.
fn speaker_preset(presets: &BTreeMap<String, BTreeMap<u32, Vec<String>>>) -> Result<&String> {
    let lower = |n: &String| n.to_lowercase();
    if presets.len() == 1 {
        let (name, _) = presets.iter().next().ok_or_else(|| anyhow!("no preset"))?;
        let n = lower(name);
        let hp_suffix = n.strip_suffix("hp").is_some_and(|head| !head.ends_with(|c: char| c.is_ascii_lowercase()));
        if n.contains("headphone") || hp_suffix {
            bail!("the only DAX2 preset, {name}, is the headphone output's");
        }
        return Ok(name);
    }
    let speaker: Vec<&String> =
        presets.keys().filter(|n| lower(n).contains("spk") || lower(n).contains("speaker")).collect();
    match speaker.as_slice() {
        [one] => Ok(one),
        _ => bail!("several DAX2 presets and none named for the speaker: {:?}", presets.keys().collect::<Vec<_>>()),
    }
}

enum Value {
    Number(i32),
    Blob(Vec<i32>),
}

/// One HKR value's PROPVARIANT: u16 type, 8-byte header, then 32-bit values.
fn decode(name: &str, index: u32, data: &[String]) -> Result<Value> {
    let blob = data
        .iter()
        .filter(|x| !x.is_empty())
        .map(|x| u8::from_str_radix(x.trim_start_matches("0x").trim_start_matches("0X"), 16))
        .collect::<Result<Vec<u8>, _>>()
        .map_err(|_| anyhow!("{name} value {index} is not hex bytes"))?;
    if blob.len() < 12 || (blob.len() - 8) % 4 != 0 {
        bail!("{name} value {index} has {} bytes, not a PROPVARIANT of 32-bit values", blob.len());
    }
    let ints: Vec<i32> = blob[8..].as_chunks::<4>().0.iter().map(|&c| i32::from_le_bytes(c)).collect();
    match u16::from_le_bytes([blob[0], blob[1]]) {
        VT_I4 => Ok(Value::Number(ints[0])),
        VT_BLOB => Ok(Value::Blob(ints)),
        vt => bail!("{name} value {index} has PROPVARIANT type {vt:#x}"),
    }
}

/// A band blob: n, then n-prefixed arrays of n values; the first array is a rising frequency grid.
fn grid(name: &str, ints: &[i32]) -> Result<(Vec<i32>, Vec<Vec<i32>>)> {
    let bad = || anyhow!("{name} audio-optimizer-bands does not split into a grid and channels");
    let n = usize::try_from(*ints.first().ok_or_else(bad)?).map_err(|_| bad())?;
    if n == 0 {
        return Err(bad());
    }
    let mut arrays = Vec::new();
    let mut rest = &ints[1..];
    while !rest.is_empty() {
        if usize::try_from(rest[0]).ok() != Some(n) || rest.len() < n + 1 {
            return Err(bad());
        }
        arrays.push(rest[1..=n].to_vec());
        rest = &rest[n + 1..];
    }
    let hz = arrays.first().cloned().ok_or_else(bad)?;
    if hz[0] <= 0 || hz.windows(2).any(|w| w[1] <= w[0]) || arrays.len() < 3 {
        return Err(bad());
    }
    Ok((hz, arrays.split_off(1)))
}

/// The DAX2 speaker tuning an .inf binds to codec `vendor_id` (`0x10EC0294`) and `subsystem`
/// (`10431A63`); `None` when it binds none.
pub fn tuning(inf: &[u8], vendor_id: u32, subsystem: &str) -> Result<Option<Tuning>> {
    let s = read_inf(inf);
    let parts: HashSet<String> = [
        format!("ven_{:04x}", vendor_id >> 16),
        format!("dev_{:04x}", vendor_id & 0xffff),
        format!("subsys_{}", subsystem.to_lowercase()),
    ]
    .into();
    // An install that binds but fails to read is reported only when no later one reads.
    let mut failed = None;
    for (_, install) in bound_installs(&s, &parts) {
        let lines: Vec<&String> =
            addreg_sections(&s, &install).iter().flat_map(|n| s.get(n).into_iter().flatten()).collect();
        let presets = presets(&lines);
        if presets.is_empty() {
            continue;
        }
        match preset_tuning(&presets, subsystem) {
            Ok(t) => return Ok(Some(t)),
            Err(e) => failed = failed.or(Some(e)),
        }
    }
    failed.map_or(Ok(None), Err)
}

/// The speaker preset among `presets` as a tuning.
fn preset_tuning(presets: &BTreeMap<String, BTreeMap<u32, Vec<String>>>, subsystem: &str) -> Result<Tuning> {
    let name = speaker_preset(presets)?;
    let raw = &presets[name];
    if raw.keys().any(|&i| i >= DAX3_FIRST_INDEX) {
        bail!("{name} is DAX3's registry form, not a DAX2 tuning");
    }
    let mut vlldp = BTreeMap::new();
    let mut cp = BTreeMap::new();
    let Value::Blob(bands) = decode(
        name,
        AUDIO_OPTIMIZER,
        raw.get(&AUDIO_OPTIMIZER).ok_or_else(|| anyhow!("{name} has no audio-optimizer-bands"))?,
    )?
    else {
        bail!("{name} audio-optimizer-bands is a number, not a blob");
    };
    let (hz, channels) = grid(name, &bands)?;
    vlldp.insert("audio-optimizer-enable".to_owned(), vec![1.0]);
    for (ch, c) in channels.iter().take(2).enumerate() {
        vlldp.insert(format!("audio-optimizer-bands.ch_{ch:02}"), c.iter().map(|&v| f64::from(v)).collect());
    }
    for (index, key) in NUMBERS {
        if let Some(data) = raw.get(&index) {
            let Value::Number(v) = decode(name, index, data)? else {
                bail!("{name} value {index} ({key}) is a blob, not a number");
            };
            cp.insert(key.to_owned(), vec![f64::from(v)]);
        }
    }
    let profile = Profile { cp, vlldp, ..Profile::default() };
    Ok(Tuning {
        source: "Dolby".to_owned(),
        subsystem: subsystem.to_uppercase(),
        tuning_date: String::new(),
        band_freqs_hz: hz.iter().map(|&f| f64::from(f)).collect(),
        endpoints: vec![Endpoint {
            kind: "internal_speaker".to_owned(),
            channels: 2,
            profiles: [("music".to_owned(), profile)].into(),
        }],
    })
}

/// `[Version] DriverVer` as (year, month, day, version parts); empty when absent.
pub fn driver_version(inf: &[u8]) -> Vec<u32> {
    let s = read_inf(inf);
    let Some(value) = s
        .get("version")
        .into_iter()
        .flatten()
        .filter_map(|l| l.split_once('='))
        .find(|(k, _)| k.trim().eq_ignore_ascii_case("driverver"))
        .map(|(_, v)| v)
    else {
        return Vec::new();
    };
    let f = fields(value);
    let mut date: Vec<u32> = f[0].split('/').filter_map(|n| n.trim().parse().ok()).collect();
    if date.len() == 3 {
        date.rotate_right(1);
    }
    date.extend(f.get(1).into_iter().flat_map(|v| v.split('.')).filter_map(|n| n.trim().parse::<u32>().ok()));
    date
}

#[cfg(test)]
mod tests {
    use anyhow::Context;

    use super::*;

    fn hex(vt: u16, ints: &[i32]) -> String {
        let mut b = vt.to_le_bytes().to_vec();
        b.extend([0; 6]);
        for i in ints {
            b.extend(i.to_le_bytes());
        }
        b.iter().map(|x| format!("{x:02x}")).collect::<Vec<_>>().join(",")
    }

    fn utf16(text: &str) -> Vec<u8> {
        let mut b = vec![0xff, 0xfe];
        for u in text.encode_utf16() {
            b.extend(u.to_le_bytes());
        }
        b
    }

    #[test]
    fn inf_binds_speaker_preset_to_codec() -> Result<()> {
        let bands = hex(VT_BLOB, &[3, 3, 100, 1000, 10000, 3, -16, -32, 0, 3, 0, -48, -16]);
        let key = "{f112024a-fe30-42a8-80ab-8dd825a06f78}";
        let inf = format!(
            "[Version]\nDriverVer = 03/04/2021,6.0.9.1\n\
             [Manufacturer]\n%Mfg% = Models, NTamd64, NTamd64.10.0\n\
             [Models.NTamd64]\n%Dev% = Other, HDAUDIO\\FUNC_01&VEN_10EC&DEV_0256&SUBSYS_10431A63\n\
             [Models.NTamd64.10.0]\n%Dev% = Codec, HDAUDIO\\FUNC_01&VEN_10EC&DEV_0294&SUBSYS_10431A63 ; this one\n\
             [Codec.NTamd64]\nAddReg = Common\n\
             [Codec.NTamd64.Interfaces]\nAddInterface = %KSCATEGORY%, \"EAPO\", Speaker.Interface\n\
             [Speaker.Interface]\nAddReg = SpeakerReg\n\
             [Common]\nHKR,,OEMSettingsOverride,,DAX2Preset1A63_SPK,DAX2Preset1A63_HP\n\
             [SpeakerReg]\n\
             HKR,InterfaceSetting\\DAX2Preset1A63_SPK\\EP\\0,\"{key},18\",0x00000001,{bands}\n\
             HKR,InterfaceSetting\\DAX2Preset1A63_SPK\\EP\\0,\"{key},9\",0x00000001,{}\n\
             HKR,InterfaceSetting\\DAX2Preset1A63_HP\\EP\\0,\"{key},18\",0x00000001,\\\n{bands}\n\
             [Strings]\nMfg = \"Realtek\"\nDev = \"Realtek, Audio\"\nKSCATEGORY = \"{{6994AD04}}\"\n",
            hex(VT_I4, &[1]),
        );
        let raw = utf16(&inf);
        assert!(mentions_dax2(&raw));
        assert_eq!(driver_version(&raw), vec![2021, 3, 4, 6, 0, 9, 1]);
        assert_eq!(tuning(&raw, 0x10EC_0294, "10430000")?, None);
        let t = tuning(&raw, 0x10EC_0294, "10431a63")?.context("codec not bound")?;
        assert_eq!((t.source.as_str(), t.band_freqs_hz.as_slice()), ("Dolby", &[100.0, 1000.0, 10000.0][..]));
        let p = &t.endpoints[0].profiles["music"];
        assert_eq!(p.vlldp["audio-optimizer-bands.ch_00"], vec![-16.0, -32.0, 0.0]);
        assert_eq!(p.vlldp["audio-optimizer-bands.ch_01"], vec![0.0, -48.0, -16.0]);
        assert_eq!(p.cp["volume-leveler-enable"], vec![1.0]);
        Ok(())
    }
}
