//! Mirroring a phone's screen for `umbriel-link-mirror`: the request, the wait for the user's consent on the phone,
//! and the pump from the phone's video stream into the viewer's socket, in the stream's own framing.

use std::collections::HashMap;
use std::os::fd::OwnedFd;
use std::time::Duration;

use link_core::identity::DeviceId;
use link_core::mirror::{MirrorReceiver, MirrorStream};
use tokio::io::AsyncWriteExt as _;
use tokio::sync::oneshot;
use tokio::task::{AbortHandle, JoinSet};
use tokio::time::Instant;

/// Android asks the user every time; they may take a while to find the phone.
pub const CONSENT_TIMEOUT: Duration = Duration::from_secs(60);

pub struct Opened {
    /// The viewer's end of a socket carrying the phone's access units.
    pub stream: OwnedFd,
    pub width: u32,
    pub height: u32,
}

#[derive(Debug)]
pub enum Failure {
    NotConnected,
    /// Bluetooth carries tens of KB/s; video needs Wi-Fi.
    NeedsWifi,
    Busy,
    Refused(String),
    TimedOut,
    Failed(String),
}

pub type Reply = oneshot::Sender<Result<Opened, Failure>>;

#[derive(Default)]
pub struct Mirrors {
    waiting: HashMap<DeviceId, (Reply, Instant)>,
    running: HashMap<DeviceId, AbortHandle>,
    pumps: JoinSet<DeviceId>,
}

impl Mirrors {
    pub fn busy(&self, id: &DeviceId) -> bool {
        self.waiting.contains_key(id) || self.running.contains_key(id)
    }

    pub fn wait(&mut self, id: DeviceId, reply: Reply) {
        self.waiting.insert(id, (reply, Instant::now() + CONSENT_TIMEOUT));
    }

    pub fn deadline(&self) -> Option<Instant> {
        self.waiting.values().map(|(_, deadline)| *deadline).min()
    }

    /// Answers the requests whose consent timed out, and returns their phones, which are told to stop.
    pub fn expire(&mut self) -> Vec<DeviceId> {
        let now = Instant::now();
        let expired: Vec<DeviceId> =
            self.waiting.iter().filter(|(_, (_, deadline))| *deadline <= now).map(|(id, _)| id.clone()).collect();
        for id in &expired {
            self.fail(id, Failure::TimedOut);
        }
        expired
    }

    pub fn fail(&mut self, id: &DeviceId, failure: Failure) {
        if let Some((reply, _)) = self.waiting.remove(id) {
            drop(reply.send(Err(failure)));
        }
    }

    /// The phone stopped: a waiting request is refused with its reason, a running mirror ends.
    pub fn stopped(&mut self, id: &DeviceId, reason: Option<String>) {
        self.fail(id, Failure::Refused(reason.unwrap_or_else(|| "the phone stopped mirroring".to_owned())));
        if let Some(pump) = self.running.remove(id) {
            pump.abort();
        }
    }

    /// The phone's video stream: handed to the request waiting for it, or refused.
    pub fn stream(&mut self, from: DeviceId, stream: &MirrorStream) {
        let Some(mut receiver) = stream.take() else { return };
        let Some((reply, _)) = self.waiting.remove(&from) else {
            log::info!("{from}: refusing a mirror stream nobody asked for");
            return receiver.stop();
        };
        let (ours, theirs) = match std::os::unix::net::UnixStream::pair() {
            Ok(pair) => pair,
            Err(error) => return drop(reply.send(Err(Failure::Failed(error.to_string())))),
        };
        let (width, height) = (receiver.width, receiver.height);
        let ours = ours.set_nonblocking(true).and_then(|()| tokio::net::UnixStream::from_std(ours));
        let ours = match ours {
            Ok(ours) => ours,
            Err(error) => return drop(reply.send(Err(Failure::Failed(error.to_string())))),
        };
        if reply.send(Ok(Opened { stream: theirs.into(), width, height })).is_err() {
            return receiver.stop();
        }
        log::info!("{from}: mirroring at {width}x{height}");
        let id = from.clone();
        let pump = self.pumps.spawn(async move {
            if let Err(error) = pump(&mut receiver, ours).await {
                log::info!("{id}: mirroring ended: {error}");
            }
            receiver.stop();
            id
        });
        self.running.insert(from, pump);
    }

    /// The next pump to end, with its phone, which is told to stop in case the viewer closed first.
    pub async fn ended(&mut self) -> Option<DeviceId> {
        let id = match self.pumps.join_next().await? {
            Ok(id) => id,
            Err(join) => {
                if !join.is_cancelled() {
                    log::warn!("mirror pump: {join}");
                }
                return None;
            }
        };
        self.running.remove(&id);
        Some(id)
    }
}

async fn pump(receiver: &mut MirrorReceiver, mut viewer: tokio::net::UnixStream) -> anyhow::Result<()> {
    while let Some(unit) = receiver.next().await? {
        let mut header = [0; 13];
        header[..4].copy_from_slice(&u32::try_from(unit.data.len())?.to_be_bytes());
        header[4..12].copy_from_slice(&unit.pts_us.to_be_bytes());
        header[12] = unit.flags;
        viewer.write_all(&header).await?;
        viewer.write_all(&unit.data).await?;
    }
    Ok(())
}
