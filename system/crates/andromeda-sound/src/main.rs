use std::path::{Path, PathBuf};
use std::process::Command;
use std::{env, fs};

use andromeda_sound::chain;
use anyhow::{Context, Result, bail};

const USAGE: &str = "usage: andromeda-sound import-dolby [--xml FILE | --windows DIR] [-o FILE]
       andromeda-sound apply [--profile NAME] [--tuning FILE] [--dsp FILE] [--target SINK] [--loudness DB] [--bass 0-10] [-o FILE]
       andromeda-sound off";
/// Defaults that make laptop speakers sound fuller and louder than the flat Dolby curve.
const DEFAULT_LOUDNESS_DB: f32 = 4.0;
const DEFAULT_BASS: f32 = 5.0;
const DSP_PATH: &str = "/usr/lib/ladspa/andromeda-dsp.so";

fn main() -> Result<()> {
    let mut args = env::args().skip(1);
    match args.next().as_deref() {
        Some("import-dolby") => import(args),
        Some("apply") => apply(args),
        Some("off") => {
            // Give the user's volume back to the speaker before the chain goes, so it never plays at 100 %.
            if let (Some(vol), Ok(speaker)) = (sink_volume(chain::SINK_NAME), speaker_sink()) {
                pactl(&["set-sink-volume", &speaker, &vol])?;
                pactl(&["set-default-sink", &speaker])?;
            }
            let conf = conf_path()?;
            if conf.exists() {
                fs::remove_file(&conf)?;
            }
            systemctl(&["restart", "filter-chain.service"])
        }
        _ => bail!(USAGE),
    }
}

fn apply(mut args: impl Iterator<Item = String>) -> Result<()> {
    let (mut profile, mut tuning, mut dsp, mut target, mut out) = ("music".to_owned(), None, default_dsp(), None, None);
    let (mut loudness, mut bass) = (DEFAULT_LOUDNESS_DB, DEFAULT_BASS);
    while let Some(a) = args.next() {
        let v = args.next().context(USAGE)?;
        match a.as_str() {
            "--profile" => profile = v,
            "--tuning" => tuning = Some(PathBuf::from(v)),
            "--dsp" => dsp = v,
            "--target" => target = Some(v),
            "--loudness" => loudness = v.parse().context("--loudness takes dB")?,
            "--bass" => bass = v.parse().context("--bass takes 0-10")?,
            "-o" => out = Some(PathBuf::from(v)),
            _ => bail!(USAGE),
        }
    }
    let tuning = if let Some(t) = tuning {
        t
    } else {
        let stem = codec_stem()?;
        data_dir()?.join(format!("{}.toml", stem.rsplit('_').next().unwrap_or_default()))
    };
    let t: andromeda_sound::Tuning = toml::from_str(
        &fs::read_to_string(&tuning).with_context(|| format!("read {}; run import-dolby first", tuning.display()))?,
    )?;
    let target = match target {
        Some(t) => t,
        None => speaker_sink()?,
    };
    let conf = chain::filter_chain_conf(
        &t,
        &chain::Settings {
            profile: &profile,
            dsp_path: &dsp,
            target: &target,
            loudness_db: loudness.clamp(0.0, 12.0),
            bass: bass.clamp(0.0, 10.0),
        },
    )?;
    if let Some(out) = out {
        return Ok(fs::write(out, conf)?);
    }
    let path = conf_path()?;
    if let Some(dir) = path.parent() {
        fs::create_dir_all(dir)?;
    }
    // The user's volume must sit before the regulator, as with Dolby: the chain's sink carries it
    // and the hardware stays at 100 %. On a profile switch the chain already holds it.
    let vol = sink_volume(chain::SINK_NAME).or_else(|| sink_volume(&target)).unwrap_or_else(|| "50%".to_owned());
    fs::write(&path, conf)?;
    systemctl(&["enable", "filter-chain.service"])?;
    systemctl(&["restart", "filter-chain.service"])?;
    wait_sink(chain::SINK_NAME)?;
    pactl(&["set-sink-volume", chain::SINK_NAME, &vol])?;
    pactl(&["set-default-sink", chain::SINK_NAME])?;
    pactl(&["set-sink-volume", &target, "100%"])?;
    println!("{}", path.display());
    Ok(())
}

/// The installed library, else the one built next to this binary (`cargo build --release`).
fn default_dsp() -> String {
    let local = env::current_exe().ok().and_then(|e| e.parent().map(|d| d.join("libandromeda_dsp.so")));
    match local {
        Some(l) if !Path::new(DSP_PATH).exists() && l.exists() => l.display().to_string(),
        _ => DSP_PATH.to_owned(),
    }
}

/// The internal speaker sink; UCM names it `…__Speaker__sink`.
fn speaker_sink() -> Result<String> {
    let out = Command::new("pactl").args(["list", "short", "sinks"]).output().context("run pactl")?;
    String::from_utf8_lossy(&out.stdout)
        .lines()
        .filter_map(|l| l.split('\t').nth(1))
        .find(|n| !n.starts_with(chain::SINK_NAME) && n.to_ascii_lowercase().contains("speaker"))
        .map(str::to_owned)
        .context("no speaker sink; pass --target")
}

/// First channel's volume as `pactl` prints it (`45%`), or None when the sink does not exist.
fn sink_volume(sink: &str) -> Option<String> {
    let out = Command::new("pactl").args(["get-sink-volume", sink]).output().ok()?;
    if !out.status.success() {
        return None;
    }
    String::from_utf8_lossy(&out.stdout).split_whitespace().find(|w| w.ends_with('%')).map(str::to_owned)
}

fn wait_sink(sink: &str) -> Result<()> {
    for _ in 0..50 {
        if sink_volume(sink).is_some() {
            return Ok(());
        }
        std::thread::sleep(std::time::Duration::from_millis(100));
    }
    bail!("{sink} did not appear within 5 s; see journalctl --user -u filter-chain.service")
}

fn pactl(args: &[&str]) -> Result<()> {
    let st = Command::new("pactl").args(args).status().context("run pactl")?;
    if !st.success() {
        bail!("pactl {} failed", args.join(" "));
    }
    Ok(())
}

fn systemctl(args: &[&str]) -> Result<()> {
    let st = Command::new("systemctl").arg("--user").args(args).status().context("run systemctl")?;
    if !st.success() {
        bail!("systemctl --user {} failed", args.join(" "));
    }
    Ok(())
}

fn conf_path() -> Result<PathBuf> {
    let base = env::var_os("XDG_CONFIG_HOME")
        .map(PathBuf::from)
        .or_else(|| env::var_os("HOME").map(|h| PathBuf::from(h).join(".config")))
        .context("no HOME")?;
    Ok(base.join("pipewire/filter-chain.conf.d/andromeda-speakers.conf"))
}

fn import(mut args: impl Iterator<Item = String>) -> Result<()> {
    let (mut xml, mut windows, mut out) = (None, None, None);
    while let Some(a) = args.next() {
        let v = args.next().context(USAGE)?;
        match a.as_str() {
            "--xml" => xml = Some(PathBuf::from(v)),
            "--windows" => windows = Some(PathBuf::from(v)),
            "-o" => out = Some(PathBuf::from(v)),
            _ => bail!(USAGE),
        }
    }
    let xml = if let Some(p) = xml {
        p
    } else {
        let stem = codec_stem()?;
        let roots = windows.map_or_else(mounted_windows_roots, |w| vec![w]);
        roots.iter().find_map(|r| find_driver_file(r, &stem)).with_context(|| {
            format!("no {stem}*.xml in a Windows driver store; mount the Windows partition or pass --xml")
        })?
    };
    let bytes = fs::read(&xml).with_context(|| format!("read {}", xml.display()))?;
    let tuning = andromeda_sound::parse(std::str::from_utf8(&bytes).context("tuning file is not UTF-8")?)?;
    let out = match out {
        Some(o) => o,
        None => data_dir()?.join(format!("{}.toml", tuning.subsystem)),
    };
    if let Some(dir) = out.parent() {
        fs::create_dir_all(dir)?;
    }
    fs::write(&out, toml::to_string(&tuning)?)?;
    println!("{}", out.display());
    Ok(())
}

/// The analog codec: Intel (0x8086) entries are HDMI.
fn codec_stem() -> Result<String> {
    for dev in fs::read_dir("/sys/bus/hdaudio/devices")?.flatten() {
        let read = |f: &str| {
            fs::read_to_string(dev.path().join(f))
                .ok()
                .and_then(|s| u32::from_str_radix(s.trim().trim_start_matches("0x"), 16).ok())
        };
        if let (Some(vendor), Some(subsys)) = (read("vendor_id"), read("subsystem_id"))
            && vendor >> 16 != 0x8086
        {
            return Ok(andromeda_sound::driver_file_stem(vendor, subsys));
        }
    }
    bail!("no analog HDA codec in /sys/bus/hdaudio/devices")
}

fn mounted_windows_roots() -> Vec<PathBuf> {
    let user = env::var("USER").unwrap_or_default();
    fs::read_dir(Path::new("/run/media").join(user))
        .into_iter()
        .flatten()
        .flatten()
        .map(|d| d.path())
        .filter(|p| p.join("Windows").is_dir())
        .collect()
}

fn find_driver_file(root: &Path, stem: &str) -> Option<PathBuf> {
    let store = root.join("Windows/System32/DriverStore/FileRepository");
    let want = stem.to_ascii_uppercase();
    fs::read_dir(store)
        .ok()?
        .flatten()
        .filter(|d| d.file_name().to_string_lossy().to_ascii_lowercase().starts_with("dax3_ext"))
        .find_map(|d| {
            fs::read_dir(d.path()).ok()?.flatten().map(|f| f.path()).find(|f| {
                let n = f.file_name().map(|n| n.to_string_lossy().to_ascii_uppercase()).unwrap_or_default();
                n.starts_with(&want) && f.extension().is_some_and(|e| e.eq_ignore_ascii_case("xml"))
            })
        })
}

fn data_dir() -> Result<PathBuf> {
    let base = env::var_os("XDG_DATA_HOME")
        .map(PathBuf::from)
        .or_else(|| env::var_os("HOME").map(|h| PathBuf::from(h).join(".local/share")))
        .context("no HOME")?;
    Ok(base.join("andromeda/sound/speakers"))
}
