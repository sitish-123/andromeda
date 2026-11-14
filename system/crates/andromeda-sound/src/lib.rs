//! Imports a Dolby DAX3 speaker tuning file (from the user's own Windows driver) into a
//! profile that the `PipeWire` filter-chain generator reads.

use std::collections::BTreeMap;

use anyhow::{Context, Result, bail};
use quick_xml::Reader;
use quick_xml::events::{BytesStart, Event};
use serde::{Deserialize, Serialize};

/// Real files are about 90 KiB; anything far larger is not a tuning file.
pub const MAX_XML_BYTES: usize = 4 << 20;

#[derive(Debug, Serialize, Deserialize, PartialEq)]
pub struct Tuning {
    /// HDA codec subsystem id, upper-case hex, for example `10431A63`.
    pub subsystem: String,
    pub tuning_date: String,
    /// Centre frequencies of the 20 Dolby bands at 48 kHz.
    pub band_freqs_hz: Vec<f64>,
    pub endpoints: Vec<Endpoint>,
}

#[derive(Debug, Serialize, Deserialize, PartialEq)]
pub struct Endpoint {
    pub kind: String,
    pub channels: u32,
    pub profiles: BTreeMap<String, Profile>,
}

/// Raw Dolby values, keyed by element path inside the section (`audio-optimizer-bands.ch_00`).
/// Gains and thresholds are in 1/16 dB; presets are already resolved.
#[derive(Debug, Default, Serialize, Deserialize, PartialEq)]
pub struct Profile {
    pub cp: BTreeMap<String, Vec<f64>>,
    pub vlldp: BTreeMap<String, Vec<f64>>,
    /// Enabled filters only.
    pub speaker_peq: Vec<PeqFilter>,
}

#[derive(Debug, Serialize, Deserialize, PartialEq)]
pub struct PeqFilter {
    pub speaker: u32,
    /// DAX3 filter type code as written in the file.
    pub kind: u32,
    pub freq_hz: f64,
    pub gain_db: f64,
    pub q: Option<f64>,
    /// Shelf slope.
    pub slope: Option<f64>,
}

#[derive(Clone, Copy, PartialEq)]
enum Section {
    Cp,
    Vlldp,
}

struct Pending {
    endpoint: usize,
    profile: String,
    section: Section,
    key: String,
    preset: String,
}

pub fn parse(xml: &str) -> Result<Tuning> {
    if xml.len() > MAX_XML_BYTES {
        bail!("tuning file is {} bytes, limit {MAX_XML_BYTES}", xml.len());
    }
    let mut reader = Reader::from_str(xml);
    let mut stack: Vec<String> = Vec::new();
    let mut constants: BTreeMap<String, Vec<f64>> = BTreeMap::new();
    let mut tuning = Tuning {
        subsystem: String::new(),
        tuning_date: String::new(),
        band_freqs_hz: Vec::new(),
        endpoints: Vec::new(),
    };
    let mut profile: Option<(String, Profile)> = None;
    let mut section: Option<(Section, usize)> = None;
    let mut pending: Vec<Pending> = Vec::new();

    loop {
        let (e, empty) = match reader.read_event().context("tuning xml")? {
            Event::Start(e) => (e, false),
            Event::Empty(e) => (e, true),
            Event::End(e) => {
                let name = String::from_utf8_lossy(e.local_name().as_ref()).into_owned();
                match name.as_str() {
                    "tuning-cp" | "tuning-vlldp" => section = None,
                    "profile" => {
                        let (name, p) = profile.take().context("profile end without start")?;
                        tuning.endpoints.last_mut().context("profile outside endpoint")?.profiles.insert(name, p);
                    }
                    _ => {}
                }
                stack.pop();
                continue;
            }
            Event::Eof => break,
            _ => continue,
        };
        let name = String::from_utf8_lossy(e.local_name().as_ref()).into_owned();
        let parent = stack.last().map_or("", String::as_str);
        match (parent, name.as_str()) {
            ("constant", _) => {
                let v = attr(&e, "fs_48000")?.or(attr(&e, "target")?).unwrap_or_default();
                constants.insert(name.clone(), numbers(&v)?);
            }
            ("device_data", "tuning_date") => tuning.tuning_date = attr(&e, "value")?.unwrap_or_default(),
            ("setting", "security-key") => tuning.subsystem = subsystem_of(&attr(&e, "value")?.unwrap_or_default()),
            (_, "endpoint") => tuning.endpoints.push(Endpoint {
                kind: attr(&e, "type")?.unwrap_or_default(),
                channels: attr(&e, "total_count")?.unwrap_or_default().parse().unwrap_or(0),
                profiles: BTreeMap::new(),
            }),
            (_, "profile") => profile = Some((attr(&e, "type")?.unwrap_or_default(), Profile::default())),
            (_, "tuning-cp") => section = Some((Section::Cp, stack.len() + 1)),
            (_, "tuning-vlldp") => section = Some((Section::Vlldp, stack.len() + 1)),
            ("speaker-peq-filters", "filter") => {
                if attr(&e, "enabled")?.as_deref() == Some("1") {
                    let num = |k| -> Result<Option<f64>> {
                        attr(&e, k)?.map(|v| v.trim().parse().context("peq number")).transpose()
                    };
                    let int =
                        |k| -> Result<u32> { attr(&e, k)?.map_or(Ok(0), |v| v.trim().parse().context("peq integer")) };
                    let p = profile.as_mut().context("filter outside profile")?;
                    p.1.speaker_peq.push(PeqFilter {
                        speaker: int("speaker")?,
                        kind: int("type")?,
                        freq_hz: num("f0")?.context("peq f0")?,
                        gain_db: num("gain")?.unwrap_or(0.0),
                        q: num("q")?,
                        slope: num("s")?,
                    });
                }
            }
            _ => {
                if let (Some((sec, depth)), Some((pname, p))) = (section, profile.as_mut()) {
                    let key = stack[depth..].iter().chain([&name]).cloned().collect::<Vec<_>>().join(".");
                    let map = if sec == Section::Cp { &mut p.cp } else { &mut p.vlldp };
                    if let Some(v) = attr(&e, "value")?.filter(|v| !v.is_empty()) {
                        map.insert(key, numbers(&v)?);
                    } else if let Some(preset) = attr(&e, "preset")? {
                        pending.push(Pending {
                            endpoint: tuning.endpoints.len().saturating_sub(1),
                            profile: pname.clone(),
                            section: sec,
                            key,
                            preset,
                        });
                    }
                }
            }
        }
        if !empty {
            stack.push(name);
        }
    }

    for p in pending {
        let values = constants.get(&p.preset).with_context(|| format!("unknown preset {}", p.preset))?.clone();
        let prof = tuning
            .endpoints
            .get_mut(p.endpoint)
            .and_then(|e| e.profiles.get_mut(&p.profile))
            .context("preset target")?;
        let map = if p.section == Section::Cp { &mut prof.cp } else { &mut prof.vlldp };
        map.insert(p.key, values);
    }
    tuning.band_freqs_hz = constants.remove("band_20_freq").context("no band_20_freq constant")?;
    if tuning.endpoints.is_empty() {
        bail!("no endpoint in tuning file");
    }
    Ok(tuning)
}

fn attr(e: &BytesStart, key: &str) -> Result<Option<String>> {
    for a in e.attributes() {
        let a = a.context("xml attribute")?;
        if a.key.local_name().as_ref() == key.as_bytes() {
            return Ok(Some(String::from_utf8(a.value.into_owned()).context("attribute utf-8")?));
        }
    }
    Ok(None)
}

fn numbers(v: &str) -> Result<Vec<f64>> {
    v.split(',').map(|n| n.trim().parse::<f64>().with_context(|| format!("not a number: {n:?}"))).collect()
}

fn subsystem_of(key: &str) -> String {
    key.split_once("SUBSYS_")
        .map(|(_, rest)| rest.chars().take_while(char::is_ascii_hexdigit).collect::<String>().to_ascii_uppercase())
        .unwrap_or_default()
}

/// File name the Windows driver uses for one codec, for example
/// `DEV_0294_SUBSYS_10431A63`, from sysfs `vendor_id` and `subsystem_id` (both `0x…`).
pub fn driver_file_stem(vendor_id: u32, subsystem_id: u32) -> String {
    format!("DEV_{:04X}_SUBSYS_{subsystem_id:08X}", vendor_id & 0xffff)
}

#[cfg(test)]
mod tests {
    use super::*;

    const FIXTURE: &str = r#"<?xml version="1.0" encoding="utf-8"?>
<device_data>
  <tuning_date value="01/02/2025 10:00:00"/>
  <constant>
    <band_20_freq fs_44100="1,2" fs_48000="100,1000"/>
    <flat target="0,0"/>
    <cut target="-32,-48"/>
  </constant>
  <setting><security-key value="INTELAUDIO\FUNC_01&amp;VEN_10EC&amp;DEV_0294&amp;SUBSYS_1043abcd=01,02"/></setting>
  <endpoint type="internal_speaker" total_count="2">
    <profile type="music">
      <tuning-cp>
        <ieq-enable value="1"/>
        <graphic-equalizer-bands preset="cut"/>
        <speaker-peq-filters>
          <filter speaker="1" enabled="1" type="1" f0="560" gain="-5.000000" q="5.000000"/>
          <filter speaker="0" enabled="0" type="3" f0="5000" gain="2.000000" s="1.000000"/>
        </speaker-peq-filters>
      </tuning-cp>
      <tuning-vlldp>
        <audio-optimizer-bands>
          <ch_00 value="16,-16"/>
          <ch_01 preset="flat"/>
        </audio-optimizer-bands>
        <filter_coefficients><coefficient value=""/></filter_coefficients>
      </tuning-vlldp>
    </profile>
  </endpoint>
</device_data>"#;

    #[test]
    fn imports_profile_with_presets_and_enabled_filters() -> Result<()> {
        let t = parse(FIXTURE)?;
        assert_eq!(t.subsystem, "1043ABCD");
        assert_eq!(t.band_freqs_hz, vec![100.0, 1000.0]);
        let music = &t.endpoints[0].profiles["music"];
        assert_eq!(music.cp["graphic-equalizer-bands"], vec![-32.0, -48.0]);
        assert_eq!(music.vlldp["audio-optimizer-bands.ch_00"], vec![16.0, -16.0]);
        assert_eq!(music.vlldp["audio-optimizer-bands.ch_01"], vec![0.0, 0.0]);
        assert!(!music.vlldp.contains_key("filter_coefficients.coefficient"));
        assert_eq!(
            music.speaker_peq,
            vec![PeqFilter { speaker: 1, kind: 1, freq_hz: 560.0, gain_db: -5.0, q: Some(5.0), slope: None }]
        );
        Ok(())
    }

    #[test]
    fn rejects_unknown_preset() {
        assert!(parse(&FIXTURE.replace("preset=\"flat\"", "preset=\"nope\"")).is_err());
    }
}
