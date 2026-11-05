//! Snap refreshes, read as the user. The root helper installs them with the system upgrade.

use anyhow::{Context, Result, bail};
use tokio::process::Command;

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Update {
    pub name: String,
    pub version: String,
    pub size: u64,
}

/// "92MB", "412kB", "1.2GB", "800B" (snap prints SI units) -> bytes; 0 when it does not parse.
fn parse_size(text: &str) -> u64 {
    let split = text.find(|c: char| !(c.is_ascii_digit() || c == '.')).unwrap_or(text.len());
    let Ok(number) = text[..split].parse::<f64>() else { return 0 };
    let scale = match &text[split..] {
        "kB" => 1e3,
        "MB" => 1e6,
        "GB" => 1e9,
        _ => 1.0,
    };
    // Sizes stay far below 2^53, so the cast keeps the value.
    #[allow(clippy::cast_possible_truncation, clippy::cast_sign_loss)]
    let bytes = (number * scale).round() as u64;
    bytes
}

/// `snap refresh --list`: a header, then `name version rev size publisher notes` rows.
pub fn parse(text: &str) -> Vec<Update> {
    text.lines()
        .skip_while(|line| !line.starts_with("Name "))
        .skip(1)
        .filter_map(|line| {
            let mut cols = line.split_whitespace();
            let (name, version, _rev, size) = (cols.next()?, cols.next()?, cols.next()?, cols.next()?);
            Some(Update { name: name.into(), version: version.into(), size: parse_size(size) })
        })
        .collect()
}

pub async fn check() -> Result<Vec<Update>> {
    let out = match Command::new("snap").args(["refresh", "--list"]).env("LC_ALL", "C").output().await {
        Ok(out) => out,
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Ok(Vec::new()),
        Err(e) => return Err(e).context("snap"),
    };
    if !out.status.success() {
        bail!("snap: {}", String::from_utf8_lossy(&out.stderr).trim());
    }
    Ok(parse(&String::from_utf8_lossy(&out.stdout)))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn rows_with_sizes() {
        let text = "Name     Version  Rev   Size   Publisher   Notes\n\
            firefox  133.0-2  5361  92MB   mozilla✓    -\n\
            core22   20241119 1722  1.2GB  canonical✓  base\n";
        let rows = parse(text);
        assert_eq!(rows.len(), 2);
        assert_eq!((rows[0].name.as_str(), rows[0].version.as_str(), rows[0].size), ("firefox", "133.0-2", 92_000_000));
        assert_eq!(rows[1].size, 1_200_000_000);
    }
}
