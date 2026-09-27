use std::fs::OpenOptions;
use std::io::{BufRead, BufReader, Write};
use std::path::Path;
use std::sync::{Arc, Mutex};

use anyhow::{Context, bail};
use link_core::control::{Direction, Tap};
use serde_json::json;

pub fn tap(path: &Path) -> anyhow::Result<Tap> {
    let file = Mutex::new(OpenOptions::new().create(true).append(true).open(path)?);
    Ok(Arc::new(move |direction: Direction, body: &[u8]| {
        let direction = if direction == Direction::Sent { "sent" } else { "received" };
        let line = json!({ "dir": direction, "cbor": hex::encode(body) });
        if let Ok(mut file) = file.lock()
            && let Err(error) = writeln!(file, "{line}")
        {
            log::warn!("transcript: {error}");
        }
    }))
}

pub fn check(schema: &Path, transcript: &Path) -> anyhow::Result<()> {
    let cddl = std::fs::read_to_string(schema).with_context(|| format!("reading {}", schema.display()))?;
    let (mut valid, mut invalid) = (0_u32, 0_u32);
    for (index, line) in BufReader::new(std::fs::File::open(transcript)?).lines().enumerate() {
        let entry: serde_json::Value = serde_json::from_str(&line?)?;
        let cbor = hex::decode(entry["cbor"].as_str().context("entry without cbor")?)?;
        match cddl::validate_cbor_from_slice(&cddl, &cbor, None) {
            Ok(()) => valid += 1,
            Err(error) => {
                invalid += 1;
                eprintln!("line {}: {error}", index + 1);
            }
        }
    }
    println!("{}", json!({ "valid": valid, "invalid": invalid }));
    if invalid > 0 || valid == 0 {
        bail!("{invalid} of {} messages violate {}", valid + invalid, schema.display());
    }
    Ok(())
}
