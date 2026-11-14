//! Unpacks an OEM audio driver download (.exe, .zip, .msi) with 7-Zip, keeping only the files a
//! speaker tuning can be in. Nested installers and archives are opened up to `MAX_DEPTH` deep.

use std::fs;
use std::path::{Path, PathBuf};
use std::process::Command;

use anyhow::{Context, Result, bail};

const MAX_DEPTH: u32 = 3;
/// Bytes all nested containers together may take on disk: OEM downloads run to about 1 GiB.
const MAX_UNPACK_BYTES: u64 = 4 << 30;
/// A PE resource under this is an icon or manifest, not an embedded installer.
const MIN_RESOURCE_BYTES: u64 = 65_536;
const CONTAINERS: [&str; 6] = ["cab", "zip", "7z", "exe", "msi", "rar"];

/// Unpacks `file` into the empty directory `dir`.
pub fn unpack(file: &Path, dir: &Path) -> Result<()> {
    let mut budget = MAX_UNPACK_BYTES;
    walk(file, dir, 0, &mut budget)?;
    let any = fs::read_dir(dir).is_ok_and(|mut d| d.next().is_some());
    if !any && which("innoextract") {
        // Inno Setup installers (Lenovo) are opaque to 7-Zip.
        run(Command::new("innoextract").args(["-s", "-d"]).arg(dir).arg(file))?;
    }
    Ok(())
}

/// Every folder at or under `dir`.
pub fn folders(dir: &Path) -> Vec<PathBuf> {
    let mut out = vec![dir.to_path_buf()];
    let mut i = 0;
    while i < out.len() {
        let sub: Vec<PathBuf> = fs::read_dir(&out[i])
            .into_iter()
            .flatten()
            .flatten()
            .filter(|d| d.file_type().is_ok_and(|t| t.is_dir()))
            .map(|d| d.path())
            .collect();
        out.extend(sub);
        i += 1;
    }
    out
}

fn walk(file: &Path, dir: &Path, depth: u32, budget: &mut u64) -> Result<()> {
    let mut pe = false;
    let mut entries = list(file, false);
    if entries.iter().all(|(p, s)| pick(p, *s, depth, false).is_none()) && is_pe(file) {
        entries = list(file, true);
        pe = true;
    }
    let picked: Vec<(String, u64, bool)> =
        entries.into_iter().filter_map(|(p, s)| pick(&p, s, depth, pe).map(|nested| (p, s, nested))).collect();
    if picked.is_empty() {
        return Ok(());
    }
    let need: u64 = picked.iter().filter(|e| e.2).map(|e| e.1).sum();
    if need > *budget {
        bail!("{} unpacks to more than a driver download holds", file.display());
    }
    *budget -= need;
    fs::create_dir_all(dir)?;
    let listfile = dir.with_extension("list");
    fs::write(&listfile, picked.iter().flat_map(|e| [e.0.as_str(), "\n"]).collect::<String>())?;
    let mut cmd = Command::new("7z");
    cmd.args(["x", "-y", "-spd", "-scsUTF-8", "-bso0", "-bsp0"]);
    if pe {
        cmd.arg("-tPE");
    }
    let status = run(cmd.arg(format!("-o{}", dir.display())).arg(file).arg(format!("@{}", listfile.display())));
    let _ = fs::remove_file(&listfile);
    status?;
    for (name, _, _) in picked.iter().filter(|e| e.2) {
        let inner = dir.join(name);
        let sub = PathBuf::from(format!("{}.d", inner.display()));
        let r = walk(&inner, &sub, depth + 1, budget);
        let _ = fs::remove_file(&inner);
        r?;
    }
    Ok(())
}

/// Whether to extract an entry: `Some(false)` for a tuning file, `Some(true)` for a container to open.
fn pick(path: &str, size: u64, depth: u32, pe: bool) -> Option<bool> {
    let p = path.replace('\\', "/");
    if p.starts_with('/') || p.split('/').any(|c| c == "..") {
        return None;
    }
    let name = p.rsplit('/').next().unwrap_or_default().to_ascii_lowercase();
    if is_tuning_name(&name) {
        return Some(false);
    }
    let ext = name.rsplit_once('.').map(|(_, e)| e).unwrap_or_default();
    let nested = if pe { p.starts_with(".rsrc/") && size >= MIN_RESOURCE_BYTES } else { CONTAINERS.contains(&ext) };
    (nested && depth < MAX_DEPTH).then_some(true)
}

/// A Dolby XML (`…SUBSYS_xxxxxxxx….xml`, not the settings or microphone files), a Sonitude
/// archive, or an .inf that may carry a DAX2 tuning.
fn is_tuning_name(name: &str) -> bool {
    let ext = name.rsplit_once('.').map(|(_, e)| e).unwrap_or_default();
    let dolby = ext == "xml"
        && name
            .split("subsys_")
            .nth(1)
            .is_some_and(|t| t.len() >= 8 && t.bytes().take(8).all(|b| b.is_ascii_alphanumeric()))
        && !["_settings", "_dmic", "_amic"].iter().any(|x| name.contains(x));
    dolby || (name.starts_with("seapodat.") && ext == "zip") || ext == "inf"
}

/// (path, size) of each file 7-Zip lists in `file`; empty when 7-Zip cannot open it.
fn list(file: &Path, pe: bool) -> Vec<(String, u64)> {
    let mut cmd = Command::new("7z");
    cmd.args(["l", "-slt", "-y"]);
    if pe {
        cmd.arg("-tPE");
    }
    let Ok(out) = cmd.arg(file).output() else { return Vec::new() };
    let text = String::from_utf8_lossy(&out.stdout);
    let Some((_, body)) = text.split_once("\n----------\n") else { return Vec::new() };
    body.split("\n\n")
        .filter_map(|block| {
            let field = |k: &str| block.lines().find_map(|l| l.strip_prefix(k).map(str::to_owned));
            let folder = field("Folder = ").is_some_and(|f| f == "+")
                || field("Attributes = ").is_some_and(|a| a.starts_with('D'));
            let path = field("Path = ")?;
            (!folder).then(|| (path, field("Size = ").and_then(|s| s.parse().ok()).unwrap_or(0)))
        })
        .collect()
}

fn is_pe(file: &Path) -> bool {
    fs::read(file).is_ok_and(|b| b.starts_with(b"MZ"))
}

fn which(tool: &str) -> bool {
    Command::new(tool).arg("--version").output().is_ok()
}

fn run(cmd: &mut Command) -> Result<()> {
    let out = cmd.output().with_context(|| format!("run {}", cmd.get_program().to_string_lossy()))?;
    if !out.status.success() {
        bail!(
            "{} failed: {}",
            cmd.get_program().to_string_lossy(),
            String::from_utf8_lossy(&out.stderr).lines().last().unwrap_or_default()
        );
    }
    Ok(())
}
