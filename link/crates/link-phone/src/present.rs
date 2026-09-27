//! The phone as the app runs it: a present session through `link_core::client`, shares and files both ways.

use std::path::PathBuf;
use std::time::Duration;

use anyhow::{Context, bail};
use link_core::client::{self, Client, ClientEvent};
use link_core::identity::DeviceId;
use link_core::inbox::Inbox;
use link_core::phone::Phone;
use link_core::proto::message::{Message, NetworkKind, Share, ShareKind, Status};
use link_core::reach::Via;
use link_core::transfer::{LocalClip, TransferEvent};
use serde_json::{Value, json};
use tokio::io::{AsyncBufReadExt, BufReader};
use tokio::signal::unix::{SignalKind, signal};

use crate::{OnOffer, files};

/// Stays present until SIGTERM, SIGINT, or `seconds`, printing every event as a JSON line. Each stdin line is a share
/// (`<text|link> <text>`) or a transfer command (`send <path>...`, `accept|decline|cancel <transfer>`).
pub struct Options {
    pub seconds: Option<u64>,
    pub on_offer: OnOffer,
    pub status: Option<Status>,
}

pub async fn hold(phone: Phone, inbox: Inbox, id: DeviceId, options: Options) -> anyhow::Result<()> {
    let Options { seconds, on_offer, status } = options;
    let (client, actor, mut events) = client::client(phone, inbox);
    let drive = async move {
        if let Some(status) = status {
            client.set_status(status).await?;
        }
        client.set_present(true).await?;
        let mut terminate = signal(SignalKind::terminate())?;
        let mut lines = BufReader::new(tokio::io::stdin()).lines();
        let mut stdin_open = true;
        let deadline = seconds.map(|seconds| tokio::time::Instant::now() + Duration::from_secs(seconds));
        loop {
            tokio::select! {
                Some(event) = events.recv() => {
                    println!("{}", describe(&event));
                    answer(&client, &event, on_offer).await;
                }
                line = lines.next_line(), if stdin_open => match line? {
                    Some(line) => println!("{}", command_line(&client, &id, &line).await),
                    None => stdin_open = false,
                },
                _ = terminate.recv() => break,
                _ = tokio::signal::ctrl_c() => break,
                () = sleep_until(deadline) => break,
            }
        }
        anyhow::Ok(())
    };
    let ((), result) = tokio::join!(actor.run(), drive);
    result
}

async fn answer(client: &Client, event: &ClientEvent, on_offer: OnOffer) {
    let ClientEvent::Transfer(TransferEvent::Offered { id, .. }) = event else { return };
    let accept = match on_offer {
        OnOffer::Accept => true,
        OnOffer::Decline => false,
        OnOffer::Ask => return,
    };
    if let Err(error) = client.decide(*id, accept).await {
        log::warn!("answering {id}: {error}");
    }
}

/// Shares once on demand and prints the outcome.
pub async fn share(phone: Phone, inbox: Inbox, id: DeviceId, share: Share) -> anyhow::Result<()> {
    let (client, actor, _events) = client::client(phone, inbox);
    let (kind, bytes) = (share.kind.as_str(), share.text.len());
    let send = async move { client.share(id, share).await };
    let ((), result) = tokio::join!(actor.run(), send);
    result.context("sharing")?;
    println!("{}", json!({ "shared": kind, "bytes": bytes }));
    Ok(())
}

/// Sends `share` without checking it first, as a hostile phone would, and prints what the desktop answered.
pub async fn share_unchecked(mut phone: Phone, id: DeviceId, share: Share) -> anyhow::Result<()> {
    let mut session = phone.connect(&id).await.context("connecting")?;
    session.control.send(Message::Share(share)).await?;
    let line = match session.control.recv().await {
        Ok(Message::ShareAck(_)) => json!({ "accepted": true }),
        Ok(other) => bail!("desktop answered {}", other.kind()),
        Err(error) => json!({ "accepted": false, "error": error.to_string() }),
    };
    session.close();
    phone.finish().await;
    println!("{line}");
    Ok(())
}

async fn command_line(client: &Client, id: &DeviceId, line: &str) -> Value {
    let (verb, rest) = line.split_once(' ').unwrap_or((line, ""));
    let result = match verb {
        "send" => send_line(client, id, rest).await,
        "clip" => clip_line(client, rest).await,
        "status" => match parse_status(&rest.replace(' ', ",")) {
            Ok(status) => {
                client.set_status(status).await.map(|()| json!({ "event": "status-set" })).map_err(Into::into)
            }
            Err(error) => Err(error),
        },
        "pull" => pull_line(client, id, rest).await,
        "accept" | "decline" => match files::parse_transfer(rest) {
            Ok(transfer) => client.decide(transfer, verb == "accept").await.map_err(Into::into),
            Err(error) => Err(error),
        }
        .map(|answered| json!({ "event": verb, "answered": answered })),
        "cancel" => match files::parse_transfer(rest) {
            Ok(transfer) => client.cancel_transfer(transfer).await.map_err(Into::into),
            Err(error) => Err(error),
        }
        .map(|cancelled| json!({ "event": "cancel", "cancelled": cancelled })),
        _ => return share_line(client, id, verb, rest).await,
    };
    result.unwrap_or_else(|error| json!({ "event": format!("{verb}-failed"), "error": error.to_string() }))
}

async fn send_line(client: &Client, id: &DeviceId, rest: &str) -> anyhow::Result<Value> {
    let paths: Vec<PathBuf> = rest.split_whitespace().map(PathBuf::from).collect();
    let names = files::names(&paths, &[])?;
    let sources = files::sources(&paths, names)?;
    let transfer = client.send_files(id.clone(), sources).await?;
    Ok(json!({ "event": "sending", "transfer": transfer.to_hex() }))
}

/// `<battery>,<charging 0|1>,<network>`, as `--status` and a `status` line take it.
pub fn parse_status(text: &str) -> anyhow::Result<Status> {
    let parts: Vec<&str> = text.split(',').collect();
    let [battery, charging, network] = parts[..] else { bail!("a status is <battery>,<charging 0|1>,<network>") };
    Ok(Status {
        battery: battery.parse().context("battery is 0 to 100")?,
        charging: charging == "1",
        network: NetworkKind::parse(network).context("network is wifi, cellular, ethernet, none, or other")?,
    })
}

/// `clip <path> <mime>...`: offers the file's bytes as the clipboard, never inline, so a pull is what moves them.
async fn clip_line(client: &Client, rest: &str) -> anyhow::Result<Value> {
    let mut words = rest.split_whitespace();
    let path = words.next().context("clip <path> <mime>...")?;
    let mimes: Vec<String> = words.map(str::to_owned).collect();
    let data = std::fs::File::open(path).with_context(|| format!("opening {path}"))?;
    client.offer_clip(LocalClip { mimes: mimes.clone(), text: None, data: Some(data) }).await?;
    Ok(json!({ "event": "clip-sent", "mimes": mimes }))
}

/// `pull <clip> <mime> <path>`: writes the desktop's offered clip into a file.
async fn pull_line(client: &Client, id: &DeviceId, rest: &str) -> anyhow::Result<Value> {
    let words: Vec<&str> = rest.split_whitespace().collect();
    let [clip, mime, path] = words[..] else { bail!("pull <clip> <mime> <path>") };
    let sink = std::fs::File::create(path).with_context(|| format!("creating {path}"))?;
    let bytes = client.pull_clip(id.clone(), clip.parse()?, mime.to_owned(), sink).await?;
    Ok(json!({ "event": "pulled", "clip": clip, "mime": mime, "bytes": bytes }))
}

async fn share_line(client: &Client, id: &DeviceId, kind: &str, text: &str) -> Value {
    let Some(kind) = ShareKind::parse(kind) else {
        return json!({ "event": "share-failed", "error": "a line is <text|link> <text>" });
    };
    let bytes = text.len();
    match client.share(id.clone(), Share { kind, text: text.to_owned() }).await {
        Ok(()) => json!({ "event": "shared", "kind": kind.as_str(), "bytes": bytes }),
        Err(error) => json!({ "event": "share-failed", "kind": kind.as_str(), "error": error.to_string() }),
    }
}

fn describe(event: &ClientEvent) -> Value {
    match event {
        ClientEvent::Connected { desktop, addr, via, resumed } => json!({
            "event": "connected", "desktop": desktop.id, "name": desktop.name, "addr": addr, "via": via_name(*via),
            "resumed": resumed,
        }),
        ClientEvent::Disconnected { id, reason } => json!({ "event": "disconnected", "desktop": id, "reason": reason }),
        ClientEvent::Received { from, share } => {
            json!({ "event": "received", "desktop": from, "kind": share.kind.as_str(), "text": share.text })
        }
        ClientEvent::Unpaired { id } => json!({ "event": "unpaired", "desktop": id }),
        ClientEvent::Transfer(event) => files::describe(event),
    }
}

pub fn via_name(via: Via) -> &'static str {
    match via {
        Via::LastKnown => "last-known",
        Via::Mdns => "mdns",
    }
}

async fn sleep_until(deadline: Option<tokio::time::Instant>) {
    match deadline {
        Some(deadline) => tokio::time::sleep_until(deadline).await,
        None => std::future::pending().await,
    }
}
