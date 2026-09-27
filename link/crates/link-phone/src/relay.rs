//! A man in the middle for the pairing E2E: it holds its own key, completes TLS with both the phone and the desktop,
//! and copies the control stream between them unchanged. Channel binding must make the pairing fail anyway.

use std::net::SocketAddr;

use anyhow::Context;
use link_core::identity::Identity;
use link_core::tls::ServerPin;
use link_core::transport::{self, Dialer};
use serde_json::json;

pub async fn run(listen: SocketAddr, target: SocketAddr) -> anyhow::Result<()> {
    let identity = Identity::generate()?;
    let endpoint = transport::server_endpoint(&identity, listen)?;
    println!("{}", json!({ "relay": endpoint.local_addr()? }));
    let phone = endpoint.accept().await.context("endpoint closed")?.await?;
    let desktop = Dialer::new(&identity)?.dial(target, ServerPin::Any).await?.connection;
    let (mut phone_send, mut phone_recv) = phone.accept_bi().await?;
    let (mut desktop_send, mut desktop_recv) = desktop.open_bi().await?;
    let upstream = tokio::io::copy(&mut phone_recv, &mut desktop_send);
    let downstream = tokio::io::copy(&mut desktop_recv, &mut phone_send);
    tokio::select! {
        _ = upstream => {}
        _ = downstream => {}
        _ = desktop.closed() => {}
    }
    let reason = desktop.closed().await;
    if let quinn::ConnectionError::ApplicationClosed(close) = &reason {
        phone.close(close.error_code, &close.reason);
    }
    println!("{}", json!({ "desktop_closed": reason.to_string() }));
    endpoint.wait_idle().await;
    Ok(())
}
