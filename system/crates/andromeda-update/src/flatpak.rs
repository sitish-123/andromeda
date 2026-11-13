//! Flatpak updates for the user's and the system installations, read as the user.

use anyhow::{Context, Result, bail};
use tokio::process::Command;

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Update {
    pub id: String,
    pub version: String,
    pub size: u64,
}

/// "12.3 MB", "980 bytes", "1.1 GB" (flatpak prints SI units) -> bytes; 0 when it does not parse.
fn parse_size(text: &str) -> u64 {
    let text = text.replace('\u{a0}', " ");
    let mut parts = text.split_whitespace();
    let (Some(number), unit) = (parts.next(), parts.next().unwrap_or("bytes")) else { return 0 };
    let Ok(number) = number.parse::<f64>() else { return 0 };
    let scale = match unit {
        "kB" | "KB" => 1e3,
        "MB" => 1e6,
        "GB" => 1e9,
        _ => 1.0,
    };
    // Sizes stay far below 2^53, so the cast keeps the value.
    #[allow(clippy::cast_possible_truncation, clippy::cast_sign_loss)]
    let bytes = (number * scale).round() as u64;
    bytes
}

/// `flatpak remote-ls --updates --columns=application,version,download-size` rows, tab separated.
pub fn parse(text: &str) -> Vec<Update> {
    text.lines()
        .filter_map(|line| {
            let mut cols = line.split('\t');
            let id = cols.next()?.trim();
            if id.is_empty() || id.contains(' ') {
                return None;
            }
            Some(Update {
                id: id.to_owned(),
                version: cols.next().unwrap_or("").trim().to_owned(),
                size: parse_size(cols.next().unwrap_or("")),
            })
        })
        .collect()
}

pub async fn check() -> Result<Vec<Update>> {
    let out = match Command::new("flatpak")
        .args(["remote-ls", "--updates", "--columns=application,version,download-size"])
        .env("LC_ALL", "C")
        .output()
        .await
    {
        Ok(out) => out,
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Ok(Vec::new()),
        Err(e) => return Err(e).context("flatpak"),
    };
    if !out.status.success() {
        bail!("flatpak: {}", String::from_utf8_lossy(&out.stderr).trim());
    }
    Ok(parse(&String::from_utf8_lossy(&out.stdout)))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn rows_with_sizes() {
        let rows = parse("org.gimp.GIMP\t2.10.38\t120.5\u{a0}MB\norg.freedesktop.Platform\t\t980 bytes\n\n");
        assert_eq!(rows.len(), 2);
        assert_eq!(
            (rows[0].id.as_str(), rows[0].version.as_str(), rows[0].size),
            ("org.gimp.GIMP", "2.10.38", 120_500_000)
        );
        assert_eq!(rows[1].size, 980);
    }
}
