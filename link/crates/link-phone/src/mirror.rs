//! Stands in for the app's screen capture: an Annex B H.264 file, split into access units at its access unit
//! delimiters, streamed on repeat at 30 frames a second once the desktop asks.

use std::path::PathBuf;
use std::time::Duration;

use anyhow::Context;
use link_core::client::Client;
use link_core::identity::DeviceId;
use link_core::mirror::Unit;
use link_core::proto::message::{MIRROR_CONFIG, MIRROR_KEYFRAME};

const FRAME_US: u64 = 33_333;
const FRAME: Duration = Duration::from_micros(FRAME_US);
const NAL_IDR: u8 = 5;
const NAL_SPS: u8 = 7;
const NAL_AUD: u8 = 9;

#[derive(Clone)]
pub struct Source {
    pub file: PathBuf,
    pub width: u32,
    pub height: u32,
}

/// Streams until the desktop goes or the task is aborted.
pub async fn stream(client: Client, desktop: DeviceId, source: Source) -> anyhow::Result<()> {
    let units = units(&std::fs::read(&source.file).context("reading --mirror-file")?);
    anyhow::ensure!(!units.is_empty(), "--mirror-file holds no access unit delimiters");
    let mut sender = client.start_mirror(desktop, source.width, source.height).await?;
    let mut ticker = tokio::time::interval(FRAME);
    for (index, unit) in units.iter().cycle().enumerate() {
        ticker.tick().await;
        let pts_us = u64::try_from(index)? * FRAME_US;
        sender.send(&Unit { pts_us, flags: unit.0, data: unit.1.clone() }).await?;
    }
    Ok(())
}

/// Each access unit with its flags: keyframe when it holds an IDR slice, configuration when it holds an SPS.
fn units(bytes: &[u8]) -> Vec<(u8, Vec<u8>)> {
    let starts: Vec<usize> = nal_starts(bytes).filter(|&(_, nal)| nal == NAL_AUD).map(|(at, _)| at).collect();
    starts
        .iter()
        .enumerate()
        .map(|(index, &start)| {
            let end = starts.get(index + 1).copied().unwrap_or(bytes.len());
            let unit = &bytes[start..end];
            let mut flags = 0;
            for (_, nal) in nal_starts(unit) {
                flags |= match nal {
                    NAL_IDR => MIRROR_KEYFRAME,
                    NAL_SPS => MIRROR_CONFIG,
                    _ => 0,
                };
            }
            (flags, unit.to_vec())
        })
        .collect()
}

/// Where each NAL unit's start code begins, and its type.
fn nal_starts(bytes: &[u8]) -> impl Iterator<Item = (usize, u8)> + '_ {
    (0..bytes.len().saturating_sub(3)).filter_map(move |at| {
        let long = bytes[at..].starts_with(&[0, 0, 0, 1]);
        let short = !long && bytes[at..].starts_with(&[0, 0, 1]) && (at == 0 || bytes[at - 1] != 0);
        let header = if long { at + 4 } else { at + 3 };
        (long || short).then(|| bytes.get(header).map(|nal| (at, nal & 0x1f))).flatten()
    })
}
