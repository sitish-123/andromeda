//! The phone over wireless debugging: finding it (`adb devices`, then the `_adb-tls-connect` services `adb mdns`
//! lists), pairing it once with the code its Wireless debugging screen shows, and listing and launching its apps
//! through scrcpy, which puts each app on its own virtual display in its own window.

use std::process::{Command, Stdio};

use anyhow::{Context, bail};

pub struct App {
    pub name: String,
    pub package: String,
}

pub enum Found {
    Serial(String),
    /// Not paired: the pairing service when the phone's "Pair with code" dialog is open, as `ip:port`.
    Unpaired {
        pairing: Option<String>,
    },
}

fn adb(args: &[&str]) -> anyhow::Result<String> {
    let output = Command::new("adb").args(args).stdin(Stdio::null()).output().context("running adb")?;
    let text = String::from_utf8_lossy(&output.stdout).into_owned() + &String::from_utf8_lossy(&output.stderr);
    if output.status.success() { Ok(text) } else { bail!("adb {}: {}", args.join(" "), text.trim()) }
}

/// Serials `adb devices` reports ready.
fn ready() -> anyhow::Result<Vec<String>> {
    Ok(adb(&["devices"])?
        .lines()
        .skip(1)
        .filter_map(|line| line.split_once('\t'))
        .filter(|(_, state)| state.trim() == "device")
        .map(|(serial, _)| serial.to_owned())
        .collect())
}

/// The name the phone shows for itself, as the Link app reports it.
fn device_name(serial: &str) -> String {
    adb(&["-s", serial, "shell", "settings", "get", "global", "device_name"])
        .map(|name| name.trim().to_owned())
        .unwrap_or_default()
}

/// `ip:port` of each mDNS service of `kind` `adb mdns services` lists.
fn services(kind: &str) -> Vec<String> {
    adb(&["mdns", "services"])
        .unwrap_or_default()
        .lines()
        .filter(|line| line.contains(kind))
        .filter_map(|line| line.split_whitespace().last().map(str::to_owned))
        .collect()
}

/// The phone named `name`, connecting to phones that advertise wireless debugging first; with one phone, whatever it
/// is called.
pub fn find(name: &str) -> anyhow::Result<Found> {
    let pick = |serials: Vec<String>| -> Option<String> {
        if serials.len() == 1 {
            return serials.into_iter().next();
        }
        serials.into_iter().find(|serial| device_name(serial) == name)
    };
    if let Some(serial) = pick(ready()?) {
        return Ok(Found::Serial(serial));
    }
    for target in services("_adb-tls-connect._tcp") {
        if let Err(error) = adb(&["connect", &target]) {
            log::info!("{error}");
        }
    }
    if let Some(serial) = pick(ready()?) {
        return Ok(Found::Serial(serial));
    }
    Ok(Found::Unpaired { pairing: services("_adb-tls-pairing._tcp").into_iter().next() })
}

/// Pairs with the phone's "Pair device with pairing code" dialog.
pub fn pair(target: &str, code: &str) -> anyhow::Result<()> {
    let answer = adb(&["pair", target, code])?;
    if answer.contains("Successfully paired") { Ok(()) } else { bail!("{}", answer.trim()) }
}

/// The phone's apps with a launcher entry, by name.
pub fn apps(serial: &str) -> anyhow::Result<Vec<App>> {
    let output = Command::new("scrcpy")
        .args(["-s", serial, "--list-apps"])
        .stdin(Stdio::null())
        .output()
        .context("running scrcpy; install scrcpy")?;
    let text = String::from_utf8_lossy(&output.stdout);
    let mut apps: Vec<App> = text.lines().filter_map(parse_app).collect();
    if apps.is_empty() && !output.status.success() {
        bail!("scrcpy --list-apps: {}", String::from_utf8_lossy(&output.stderr).trim());
    }
    apps.sort_by_key(|app| app.name.to_lowercase());
    Ok(apps)
}

/// One line of `scrcpy --list-apps`: ` * Name   package` for user apps, ` - Name   package` for system ones.
fn parse_app(line: &str) -> Option<App> {
    let rest = line.trim_start().strip_prefix("* ").or_else(|| line.trim_start().strip_prefix("- "))?;
    let (name, package) = rest.trim_end().rsplit_once(char::is_whitespace)?;
    package.contains('.').then(|| App { name: name.trim().to_owned(), package: package.to_owned() })
}

/// Opens `package` on a new virtual display in its own window; the window closing ends the display. Sound stays on
/// the phone: scrcpy would forward all of it, once per window.
pub fn launch(serial: &str, app: &App) -> anyhow::Result<()> {
    Command::new("scrcpy")
        .args([
            "-s",
            serial,
            "--new-display",
            &format!("--start-app={}", app.package),
            &format!("--window-title={}", app.name),
            "--no-vd-system-decorations",
            "--no-audio",
        ])
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .spawn()
        .context("running scrcpy; install scrcpy")?;
    Ok(())
}
