//! How the phone reaches a paired desktop: last-known addresses first, then mDNS.

use std::net::SocketAddr;
use std::time::Duration;

use tokio::task::JoinSet;

use crate::Error;
use crate::discovery;
use crate::store::Peer;
use crate::tls::ServerPin;
use crate::transport::{Dialed, Dialer};

/// Delay between starting successive candidates, so the most recent address usually wins without a race.
const STAGGER: Duration = Duration::from_millis(150);
pub const MDNS_WINDOW: Duration = Duration::from_secs(3);

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Via {
    LastKnown,
    Mdns,
    Bluetooth,
    /// The phone's own hotspot, at the address the desktop reported after joining it.
    Hotspot,
    /// The phone's Wi-Fi Direct group, likewise.
    WifiDirect,
}

pub struct Reached {
    pub dialed: Dialed,
    pub addr: SocketAddr,
    pub via: Via,
}

pub async fn reach(dialer: &Dialer, peer: &Peer) -> Result<Reached, Error> {
    let pin = ServerPin::Key(peer.spki()?.fingerprint());
    match race(dialer, &peer.addresses, pin).await {
        Ok((winner, addr)) => {
            return Ok(Reached { dialed: winner, addr, via: Via::LastKnown });
        }
        Err(error) => log::info!("last-known addresses of {}: {error}", peer.id),
    }
    let found = discovery::browse(MDNS_WINDOW, Some(&peer.id)).await?;
    let candidates: Vec<SocketAddr> =
        found.into_iter().filter(|desktop| desktop.id == peer.id).flat_map(|desktop| desktop.addresses).collect();
    let (winner, addr) = race(dialer, &candidates, pin).await?;
    Ok(Reached { dialed: winner, addr, via: Via::Mdns })
}

/// Dials the candidates in order, each [`STAGGER`] after the last; the first completed handshake wins.
pub async fn race(dialer: &Dialer, candidates: &[SocketAddr], pin: ServerPin) -> Result<(Dialed, SocketAddr), Error> {
    let mut attempts = JoinSet::new();
    for (index, &addr) in candidates.iter().enumerate() {
        let dialer = dialer.clone();
        let delay = STAGGER * u32::try_from(index).unwrap_or(u32::MAX);
        attempts.spawn(async move {
            tokio::time::sleep(delay).await;
            dialer.dial(addr, pin).await.map(|dialed| (dialed, addr))
        });
    }
    let mut last_error = Error::Unreachable;
    while let Some(joined) = attempts.join_next().await {
        match joined {
            Ok(Ok(winner)) => return Ok(winner),
            Ok(Err(error)) => last_error = error,
            Err(join) => log::warn!("dial task: {join}"),
        }
    }
    Err(last_error)
}
