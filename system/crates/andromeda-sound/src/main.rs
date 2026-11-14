use std::path::{Path, PathBuf};
use std::process::Command;
use std::{env, fs};

use andromeda_sound::chain;
use anyhow::{Context, Result, bail};

const USAGE: &str = "usage: andromeda-sound import-dolby [--xml FILE | --windows DIR] [-o FILE]
       andromeda-sound apply [--profile NAME] [--tuning FILE] [--dsp FILE] [--target SINK] [--loudness DB] [--bass 0-10] [-o FILE]
       andromeda-sound off
       andromeda-sound import-windows
       andromeda-sound status";
/// Defaults that make laptop speakers sound fuller and louder than the flat Dolby curve.
const DEFAULT_LOUDNESS_DB: f32 = 4.0;
const DEFAULT_BASS: f32 = 5.0;
const DSP_PATH: &str = "/usr/lib/ladspa/andromeda-dsp.so";

fn main() -> Result<()> {
    let mut args = env::args().skip(1);
    let cmd = args.next();
    // Settings and the login offer may both run a command: one writes the chain at a time.
    let _lock = if cmd.as_deref() == Some("status") { None } else { Some(lock()?) };
    match cmd.as_deref() {
        Some("import-dolby") => import(args).map(drop),
        Some("apply") => apply(args),
        Some("off") => {
            // Give the user's volume back to the speaker before the chain goes, so it never plays at 100 %.
            let speaker = speaker_sink()?;
            if let Some(vol) = sink_volume(chain::SINK_NAME).or_else(|| fs::read_to_string(volume_path().ok()?).ok()) {
                pactl(&["set-sink-volume", &speaker, vol.trim()])?;
            }
            if default_sink().as_deref() == Some(chain::SINK_NAME) {
                pactl(&["set-default-sink", &speaker])?;
            }
            // The config stays as `.off`: `status` reads the last settings from it.
            let conf = conf_path()?;
            if conf.exists() {
                fs::rename(&conf, conf.with_extension("conf.off"))?;
            }
            systemctl(&["restart", "filter-chain.service"])
        }
        Some("status") => status(),
        // Import from Windows, then turn the chain on with the user's last bass and loudness.
        Some("import-windows") => {
            let tuning: andromeda_sound::Tuning = toml::from_str(&fs::read_to_string(import(std::iter::empty())?)?)?;
            let (_, last, loudness, bass) = last_settings()?;
            let styles = styles(Some(&tuning));
            // The last style when the new tuning has it, else music, else the tuning's first style.
            let profile = [last.as_str(), "music"]
                .into_iter()
                .chain(styles.first().copied())
                .find(|p| styles.contains(p))
                .context("the imported tuning has no speaker style")?
                .to_owned();
            apply(["--profile", &profile, "--loudness", &loudness, "--bass", &bass].into_iter().map(str::to_owned))
                .context("imported the tuning, but apply failed")
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
    // No imported tuning (no Windows, or no Dolby driver): the generic chain still adds bass and loudness.
    let t: Option<andromeda_sound::Tuning> = match fs::read_to_string(&tuning) {
        Ok(text) => Some(toml::from_str(&text).with_context(|| format!("parse {}", tuning.display()))?),
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => None,
        Err(e) => return Err(e).with_context(|| format!("read {}", tuning.display())),
    };
    let target = match target {
        Some(t) => t,
        None => speaker_sink()?,
    };
    let conf = chain::filter_chain_conf(
        t.as_ref(),
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
    // The user's volume must sit before the limiter: the chain's sink carries it
    // and the hardware stays at 100 %. On a profile switch the chain already holds it.
    let vol = sink_volume(chain::SINK_NAME).or_else(|| sink_volume(&target)).unwrap_or_else(|| "50%".to_owned());
    // Move the default only when the speakers play: never take it from headphones or HDMI.
    let speakers_play = matches!(default_sink(), Some(d) if d == target || d == chain::SINK_NAME);
    fs::write(&path, conf)?;
    let _ = fs::remove_file(path.with_extension("conf.off"));
    systemctl(&["enable", "filter-chain.service"])?;
    systemctl(&["restart", "filter-chain.service"])?;
    wait_sink(chain::SINK_NAME)?;
    pactl(&["set-sink-volume", chain::SINK_NAME, &vol])?;
    if speakers_play {
        pactl(&["set-default-sink", chain::SINK_NAME])?;
    }
    // `off` restores this when the chain sink is gone and cannot report the volume.
    fs::write(volume_path()?, &vol)?;
    pactl(&["set-sink-volume", &target, "100%"])?;
    println!("{}", path.display());
    Ok(())
}

/// (enabled, profile, loudness, bass) from the header of the live config, else of the `.off` one.
fn last_settings() -> Result<(bool, String, String, String)> {
    let conf = conf_path()?;
    let enabled = conf.exists();
    let text =
        fs::read_to_string(&conf).or_else(|_| fs::read_to_string(conf.with_extension("conf.off"))).unwrap_or_default();
    let header = text.lines().next().unwrap_or_default();
    let field = |key: &str| {
        header
            .split_once(&format!("{key} "))
            .and_then(|(_, r)| r.split([' ', ';']).next())
            .map(|v| v.trim_matches('"').to_owned())
    };
    Ok((
        enabled,
        field("profile").unwrap_or_else(|| "music".to_owned()),
        field("loudness").unwrap_or_else(|| DEFAULT_LOUDNESS_DB.to_string()),
        field("bass").unwrap_or_else(|| DEFAULT_BASS.to_string()),
    ))
}

/// One `key=value` per line for the Settings card: enabled, source (`basic` or the vendor),
/// profile, loudness, bass, profiles (comma list from the imported tuning), windows (an NTFS
/// partition exists), speakers (an internal speaker sink exists).
fn status() -> Result<()> {
    let (enabled, profile, loudness, bass) = last_settings()?;
    let tuning = codec_stem().ok().and_then(|stem| {
        let path = data_dir().ok()?.join(format!("{}.toml", stem.rsplit('_').next().unwrap_or_default()));
        toml::from_str::<andromeda_sound::Tuning>(&fs::read_to_string(path).ok()?).ok()
    });
    let profiles = styles(tuning.as_ref());
    let windows = !ntfs_partitions().is_empty();
    println!("enabled={}", u8::from(enabled));
    println!("source={}", if tuning.is_some() { "Dolby" } else { "basic" });
    println!("profile={profile}");
    println!("loudness={loudness}");
    println!("bass={bass}");
    println!("profiles={}", profiles.join(","));
    println!("windows={}", u8::from(windows));
    println!("speakers={}", u8::from(speaker_sink().is_ok()));
    Ok(())
}

/// The styles a user picks; vendor files also hold "off", user slots and niche variants.
fn styles(tuning: Option<&andromeda_sound::Tuning>) -> Vec<&'static str> {
    let names =
        tuning.and_then(|t| t.endpoints.iter().find(|e| e.kind == "internal_speaker")).map(|e| &e.profiles);
    ["music", "movie", "voice", "game", "dynamic"]
        .into_iter()
        .filter(|p| names.is_some_and(|n| n.contains_key(*p)))
        .collect()
}

/// NTFS partitions on internal disks as (device, mount point or empty); USB drives are not Windows installs.
fn ntfs_partitions() -> Vec<(String, String)> {
    let Ok(out) = Command::new("lsblk").args(["-rno", "PATH,FSTYPE,HOTPLUG,MOUNTPOINT"]).output() else {
        return Vec::new();
    };
    String::from_utf8_lossy(&out.stdout)
        .lines()
        .filter_map(|l| {
            let f: Vec<&str> = l.split(' ').collect();
            // `lsblk -r` writes a space in a mount point as \x20.
            (f.len() >= 3 && f[1] == "ntfs" && f[2] == "0")
                .then(|| (f[0].to_owned(), f.get(3).unwrap_or(&"").replace("\\x20", " ")))
        })
        .collect()
}

fn default_sink() -> Option<String> {
    let out = Command::new("pactl").arg("get-default-sink").output().ok()?;
    out.status.success().then(|| String::from_utf8_lossy(&out.stdout).trim().to_owned())
}

/// Held until exit: an exclusive lock on $XDG_RUNTIME_DIR/andromeda-sound.lock.
fn lock() -> Result<fs::File> {
    let dir = env::var_os("XDG_RUNTIME_DIR").map_or_else(env::temp_dir, PathBuf::from);
    let file = fs::File::create(dir.join("andromeda-sound.lock")).context("create the lock file")?;
    rustix::fs::flock(&file, rustix::fs::FlockOperation::LockExclusive).context("lock")?;
    Ok(file)
}

fn volume_path() -> Result<PathBuf> {
    let dir = data_dir()?;
    fs::create_dir_all(&dir)?;
    Ok(dir.join("volume"))
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

/// Writes the tuning TOML and returns its path.
fn import(mut args: impl Iterator<Item = String>) -> Result<PathBuf> {
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
    let bytes = if let Some(p) = xml {
        fs::read(&p).with_context(|| format!("read {}", p.display()))?
    } else {
        let stem = codec_stem()?;
        let mut roots = windows.map_or_else(mounted_windows_roots, |w| vec![w]);
        let ours =
            if roots.iter().any(|r| find_driver_file(r, &stem).is_some()) { Vec::new() } else { mount_ntfs_ro() };
        roots.extend(ours.iter().map(|(_, at)| at.clone()));
        let read = roots
            .iter()
            .find_map(|r| find_driver_file(r, &stem))
            .map(|f| fs::read(&f).with_context(|| format!("read {}", f.display())));
        for (dev, _) in &ours {
            let _ = Command::new("udisksctl").args(["unmount", "-b", dev, "--no-user-interaction"]).output();
        }
        read.with_context(|| format!("no Windows partition with a Dolby tuning file ({stem}*.xml)"))??
    };
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
    Ok(out)
}

/// Mounts every unmounted internal NTFS partition read-only through udisks; polkit may ask for the
/// password through the session's agent. Returns (device, mount point) for the caller to unmount.
fn mount_ntfs_ro() -> Vec<(String, PathBuf)> {
    ntfs_partitions()
        .into_iter()
        .filter(|(_, at)| at.is_empty())
        .filter_map(|(dev, _)| {
            let out = Command::new("udisksctl").args(["mount", "-b", &dev, "-o", "ro"]).output().ok()?;
            let text = String::from_utf8_lossy(&out.stdout);
            let at = text.trim().rsplit_once(" at ")?.1.trim_end_matches('.').to_owned();
            Some((dev, PathBuf::from(at)))
        })
        .collect()
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

/// Mounted internal NTFS partitions that hold a Windows directory, wherever they are mounted.
fn mounted_windows_roots() -> Vec<PathBuf> {
    ntfs_partitions()
        .into_iter()
        .map(|(_, at)| PathBuf::from(at))
        .filter(|p| !p.as_os_str().is_empty() && p.join("Windows").is_dir())
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
