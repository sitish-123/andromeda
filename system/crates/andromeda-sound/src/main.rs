use std::path::{Path, PathBuf};
use std::{env, fs};

use anyhow::{Context, Result, bail};

const USAGE: &str = "usage: andromeda-sound import-dolby [--xml FILE | --windows DIR] [-o FILE]";

fn main() -> Result<()> {
    let mut args = env::args().skip(1);
    if args.next().as_deref() != Some("import-dolby") {
        bail!(USAGE);
    }
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
