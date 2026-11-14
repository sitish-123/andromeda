use std::path::{Path, PathBuf};
use std::process::Command;
use std::{env, fs};

use andromeda_sound::chain;
use anyhow::{Context, Result, bail};

const USAGE: &str = "usage: andromeda-sound import-dolby [--xml FILE | --windows DIR] [-o FILE]
       andromeda-sound apply [--profile NAME] [--tuning FILE] [--dsp FILE] [--target SINK] [-o FILE]
       andromeda-sound off";
const DSP_PATH: &str = "/usr/lib/ladspa/andromeda-dsp.so";

fn main() -> Result<()> {
    let mut args = env::args().skip(1);
    match args.next().as_deref() {
        Some("import-dolby") => import(args),
        Some("apply") => apply(args),
        Some("off") => {
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
    while let Some(a) = args.next() {
        let v = args.next().context(USAGE)?;
        match a.as_str() {
            "--profile" => profile = v,
            "--tuning" => tuning = Some(PathBuf::from(v)),
            "--dsp" => dsp = v,
            "--target" => target = Some(v),
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
    let conf = chain::filter_chain_conf(&t, &chain::Settings { profile: &profile, dsp_path: &dsp, target: &target })?;
    if let Some(out) = out {
        return Ok(fs::write(out, conf)?);
    }
    let path = conf_path()?;
    if let Some(dir) = path.parent() {
        fs::create_dir_all(dir)?;
    }
    fs::write(&path, conf)?;
    systemctl(&["enable", "filter-chain.service"])?;
    systemctl(&["restart", "filter-chain.service"])?;
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
        .find(|n| n.to_ascii_lowercase().contains("speaker"))
        .map(str::to_owned)
        .context("no speaker sink; pass --target")
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
