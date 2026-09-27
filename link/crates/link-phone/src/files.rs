//! Sending files from the headless phone: through the client as the app does, and a hostile sender that streams past
//! the size it announced.

use std::fs::File;
use std::path::PathBuf;
use std::time::Instant;

use anyhow::{Context, bail};
use link_core::client::{self, ClientEvent};
use link_core::control::write_frame;
use link_core::identity::DeviceId;
use link_core::inbox::Inbox;
use link_core::phone::Phone;
use link_core::proto::message::{FileData, FileMeta, Message, Offer, TransferId};
use link_core::transfer::{CONSENT_TIMEOUT, Source, TransferEvent};
use ring::digest;
use ring::rand::{SecureRandom, SystemRandom};
use serde_json::{Value, json};

pub const OCTET_STREAM: &str = "application/octet-stream";

/// The name each path is sent as: `--as-name` in order, else the path's own file name.
pub fn names(paths: &[PathBuf], given: &[String]) -> anyhow::Result<Vec<String>> {
    if paths.is_empty() {
        bail!("give at least one path");
    }
    paths
        .iter()
        .enumerate()
        .map(|(index, path)| match given.get(index) {
            Some(name) => unescape(name),
            None => Ok(path.file_name().context("a path without a file name")?.to_string_lossy().into_owned()),
        })
        .collect()
}

/// Decodes `%XX` escapes, so a test can send a NUL that argv cannot carry.
fn unescape(name: &str) -> anyhow::Result<String> {
    let mut bytes = Vec::with_capacity(name.len());
    let mut rest = name.as_bytes();
    while let Some((&byte, tail)) = rest.split_first() {
        if byte == b'%' && tail.len() >= 2 {
            let hex = std::str::from_utf8(&tail[..2])?;
            bytes.push(u8::from_str_radix(hex, 16).context("a % escape is two hex digits")?);
            rest = &tail[2..];
        } else {
            bytes.push(byte);
            rest = tail;
        }
    }
    Ok(String::from_utf8(bytes)?)
}

pub fn sources(paths: &[PathBuf], names: Vec<String>) -> anyhow::Result<Vec<Source>> {
    paths
        .iter()
        .zip(names)
        .map(|(path, name)| {
            let file = File::open(path).with_context(|| format!("opening {}", path.display()))?;
            Ok(Source::new(file, name, OCTET_STREAM.to_owned())?)
        })
        .collect()
}

/// Sends through the client, printing progress and then the result with its throughput.
pub async fn send(
    phone: Phone,
    inbox: Inbox,
    id: DeviceId,
    paths: &[PathBuf],
    names: Vec<String>,
) -> anyhow::Result<()> {
    let sources = sources(paths, names)?;
    let bytes: u64 = sources.iter().map(Source::size).sum();
    let (client, actor, mut events) = client::client(phone, inbox);
    let drive = async move {
        let started = Instant::now();
        let transfer = client.send_files(id, sources).await.context("sending")?;
        println!("{}", json!({ "event": "sending", "transfer": transfer.to_hex(), "bytes": bytes }));
        while let Some(event) = events.recv().await {
            let ClientEvent::Transfer(event) = event else { continue };
            println!("{}", describe(&event));
            if let TransferEvent::Finished { id, status, .. } = event
                && id == transfer
            {
                let seconds = started.elapsed().as_secs_f64();
                let line = json!({
                    "event": "result", "transfer": transfer.to_hex(), "status": status.as_str(), "bytes": bytes,
                    "seconds": seconds, "mib_per_s": f64::from(u32::try_from(bytes >> 10).unwrap_or(u32::MAX)) / 1024.0 / seconds,
                });
                println!("{line}");
                return Ok(());
            }
        }
        bail!("the client stopped")
    };
    let ((), result) = tokio::join!(actor.run(), drive);
    result
}

/// Offers the first path with its true size and hash, then streams `extra` bytes past it, as a hostile phone would.
pub async fn send_oversize(
    mut phone: Phone,
    id: DeviceId,
    paths: &[PathBuf],
    names: &[String],
    extra: u64,
) -> anyhow::Result<()> {
    let (path, name) = (&paths[0], names[0].clone());
    let bytes = std::fs::read(path)?;
    let mut transfer = TransferId([0; 16]);
    SystemRandom::new().fill(&mut transfer.0).map_err(|_| anyhow::anyhow!("no randomness"))?;
    let sha256 = digest::digest(&digest::SHA256, &bytes).as_ref().to_vec();
    let file = FileMeta { id: 0, name, size: bytes.len() as u64, mime: OCTET_STREAM.to_owned(), sha256 };
    let mut session = phone.connect(&id).await.context("connecting")?;
    session.control.send(Message::Offer(Offer { transfer, files: vec![file] })).await?;
    let reply = session.control.recv().await?;
    let Message::OfferReply(reply) = reply else { bail!("desktop answered {}", reply.kind()) };
    if !reply.accepted {
        bail!("the desktop refused the offer; enable auto-accept first");
    }
    let mut send = session.connection.open_uni().await?;
    let header = FileData { transfer, file: 0, offset: 0 };
    write_frame(&mut send, Message::FileData(header), session.control.tap().as_ref()).await?;
    let padding = vec![0; usize::try_from(extra)?];
    let written = async {
        send.write_all(&bytes).await?;
        send.write_all(&padding).await?;
        send.finish()?;
        anyhow::Ok(())
    };
    let write_error = written.await.err().map(|error| error.to_string());
    let stopped = tokio::time::timeout(CONSENT_TIMEOUT, send.stopped()).await?.ok().flatten();
    let done = session.control.recv().await?;
    let line = match done {
        Message::FileDone(done) => json!({
            "stopped_with": stopped.map(quinn::VarInt::into_inner), "write_error": write_error, "ok": done.ok,
        }),
        other => bail!("desktop answered {}", other.kind()),
    };
    session.close();
    phone.finish().await;
    println!("{line}");
    Ok(())
}

pub fn describe(event: &TransferEvent) -> Value {
    match event {
        TransferEvent::Offered { id, from, files } => {
            let files: Vec<Value> = files.iter().map(|file| json!({ "name": file.name, "size": file.size })).collect();
            json!({ "event": "offered", "transfer": id.to_hex(), "desktop": from, "files": files })
        }
        TransferEvent::Progress { id, bytes, total } => {
            json!({ "event": "progress", "transfer": id.to_hex(), "bytes": bytes, "total": total })
        }
        TransferEvent::Finished { id, peer, incoming, status, files } => json!({
            "event": "finished", "transfer": id.to_hex(), "desktop": peer, "incoming": incoming,
            "status": status.as_str(),
            "paths": files.iter().map(|file| file.path.display().to_string()).collect::<Vec<_>>(),
        }),
        TransferEvent::Busy { peer, busy } => json!({ "event": "busy", "desktop": peer, "busy": busy }),
        TransferEvent::ClipOffered { from, id, mimes, size, text } => json!({
            "event": "clip-offered", "desktop": from, "clip": id, "mimes": mimes, "size": size, "text": text,
        }),
    }
}

pub fn parse_transfer(text: &str) -> anyhow::Result<TransferId> {
    TransferId::parse_hex(text.trim()).context("not a transfer id")
}
