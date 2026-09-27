//! The phone as the app runs it: a present session through `link_core::client`, and shares both ways.

use std::time::Duration;

use anyhow::{Context, bail};
use link_core::client::{self, Client, ClientEvent};
use link_core::identity::DeviceId;
use link_core::phone::Phone;
use link_core::proto::message::{Message, Share, ShareKind};
use link_core::reach::Via;
use serde_json::{Value, json};
use tokio::io::{AsyncBufReadExt, BufReader};
use tokio::signal::unix::{SignalKind, signal};

/// Stays present until SIGTERM, SIGINT, or `seconds`, printing every event as a JSON line. Each stdin line
/// `<text|link> <text>` is shared with the desktop.
pub async fn hold(phone: Phone, id: DeviceId, seconds: Option<u64>) -> anyhow::Result<()> {
    let (client, actor, mut events) = client::client(phone);
    let drive = async move {
        client.set_present(true).await?;
        let mut terminate = signal(SignalKind::terminate())?;
        let mut lines = BufReader::new(tokio::io::stdin()).lines();
        let mut stdin_open = true;
        let deadline = seconds.map(|seconds| tokio::time::Instant::now() + Duration::from_secs(seconds));
        loop {
            tokio::select! {
                Some(event) = events.recv() => println!("{}", describe(&event)),
                line = lines.next_line(), if stdin_open => match line? {
                    Some(line) => println!("{}", share_line(&client, &id, &line).await),
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

/// Shares once on demand and prints the outcome.
pub async fn share(phone: Phone, id: DeviceId, share: Share) -> anyhow::Result<()> {
    let (client, actor, _events) = client::client(phone);
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

async fn share_line(client: &Client, id: &DeviceId, line: &str) -> Value {
    let (kind, text) = line.split_once(' ').unwrap_or((line, ""));
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
