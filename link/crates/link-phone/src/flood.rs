//! A hostile phone that sends one kind of message as fast as it can, to prove the desktop's rate limits.

use std::time::Duration;

use anyhow::Context;
use link_core::identity::DeviceId;
use link_core::phone::Phone;
use link_core::proto::message::{
    ClipOffer, FileMeta, Message, NetworkKind, Offer, Share, ShareKind, Status, TransferId,
};
use serde_json::json;

#[derive(Clone, Copy, PartialEq, Eq, clap::ValueEnum)]
pub enum Kind {
    Share,
    Offer,
    Clip,
    Status,
}

/// Sends `count` messages of `kind` back to back, then counts the answers for two seconds: acks for shares, replies
/// for offers (and how many said busy).
pub async fn flood(mut phone: Phone, id: DeviceId, kind: Kind, count: u32) -> anyhow::Result<()> {
    let mut session = phone.connect(&id).await.context("connecting")?;
    for n in 0..count {
        session.control.send(message(kind, n)).await?;
    }
    let (mut acked, mut accepted, mut busy) = (0, 0, 0);
    while let Ok(Ok(message)) = tokio::time::timeout(Duration::from_secs(2), session.control.recv()).await {
        match message {
            Message::ShareAck(_) => acked += 1,
            Message::OfferReply(reply) if reply.accepted => accepted += 1,
            Message::OfferReply(_) => busy += 1,
            _ => {}
        }
    }
    session.close();
    phone.finish().await;
    let line = json!({ "sent": count, "acked": acked, "offers_answered": accepted, "offers_busy": busy });
    println!("{line}");
    Ok(())
}

fn message(kind: Kind, n: u32) -> Message {
    match kind {
        Kind::Share => Message::Share(Share { kind: ShareKind::Text, text: format!("flood {n}") }),
        Kind::Offer => {
            let mut transfer = TransferId([0; 16]);
            transfer.0[..4].copy_from_slice(&n.to_be_bytes());
            let file = FileMeta {
                id: 0,
                name: format!("flood-{n}"),
                size: 1,
                mime: "text/plain".to_owned(),
                sha256: vec![0; 32],
            };
            Message::Offer(Offer { transfer, files: vec![file] })
        }
        Kind::Clip => Message::ClipOffer(ClipOffer {
            id: u64::from(n) + 1,
            mimes: vec!["text/plain".to_owned()],
            size: 1,
            text: Some("x".to_owned()),
        }),
        Kind::Status => Message::Status(Status {
            battery: u8::try_from(n % 101).unwrap_or(0),
            charging: false,
            network: NetworkKind::Wifi,
        }),
    }
}
