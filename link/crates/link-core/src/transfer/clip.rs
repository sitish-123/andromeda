//! Clipboard offers and pulls (`link/ARCHITECTURE.md`, Clipboard). The transfer actor serves them, since it owns the
//! bulk streams a pull is answered on.

use std::collections::HashMap;
use std::fs::File;
use std::io::Write as _;
use std::os::unix::fs::{FileExt, FileTypeExt};
use std::sync::Arc;

use link_proto::clip::{ClipState, Serve};
use link_proto::message::{ClipOffer, ClipPull, MAX_CLIP_SIZE, MAX_SHARE_LEN, Message, is_text_mime};
use link_proto::transfer::Budget;
use ring::digest;
use tokio::io::AsyncWriteExt;
use tokio::net::unix::pipe;
use tokio::sync::oneshot;
use tokio::time::Instant;

use super::{CANCELLED, CHUNK, Outbound, PROTOCOL_ERROR, TaskEnd, TransferActor, TransferEvent};
use crate::Error;
use crate::control::{STEP_TIMEOUT, write_frame};
use crate::identity::DeviceId;
use crate::session::SessionHandle;
use crate::wire::RecvStream;

/// What this side offers: the types in order of preference, and the first type's bytes, inline as text or in a file.
pub struct LocalClip {
    pub mimes: Vec<String>,
    pub text: Option<String>,
    pub data: Option<File>,
}

#[derive(Default)]
pub(super) struct Clips {
    peers: HashMap<DeviceId, ClipState>,
    /// The bytes of this side's latest offer.
    local: Option<Content>,
    /// SHA-256 of the last clip applied from a peer.
    applied: Option<Vec<u8>>,
    pulls: Vec<Pull>,
}

#[derive(Clone)]
enum Content {
    Text(Arc<str>),
    File(Arc<File>, u64),
}

struct Pull {
    peer: DeviceId,
    header: ClipPull,
    sink: Option<File>,
    reply: oneshot::Sender<Result<u64, Error>>,
    deadline: Instant,
}

impl Clips {
    pub(super) fn next_deadline(&self) -> Option<Instant> {
        self.pulls.iter().filter(|pull| pull.sink.is_some()).map(|pull| pull.deadline).min()
    }
}

impl LocalClip {
    fn content(self) -> Result<(Vec<String>, Content, Vec<u8>), Error> {
        let content = match (self.text, self.data) {
            (Some(text), _) => Content::Text(text.into()),
            (None, Some(file)) => {
                let size = file.metadata()?.len();
                Content::File(Arc::new(file), size)
            }
            (None, None) => return Err(Error::Unexpected("a clip without content")),
        };
        let hash = content.hash()?;
        Ok((self.mimes, content, hash))
    }
}

impl Content {
    fn size(&self) -> u64 {
        match self {
            Self::Text(text) => text.len() as u64,
            Self::File(_, size) => *size,
        }
    }

    fn hash(&self) -> Result<Vec<u8>, Error> {
        let mut context = digest::Context::new(&digest::SHA256);
        match self {
            Self::Text(text) => context.update(text.as_bytes()),
            Self::File(file, size) => {
                let mut buf = vec![0; CHUNK];
                let mut offset = 0;
                while offset < *size {
                    let read = file.read_at(&mut buf, offset)?;
                    if read == 0 {
                        break;
                    }
                    context.update(&buf[..read]);
                    offset += read as u64;
                }
            }
        }
        Ok(context.finish().as_ref().to_vec())
    }
}

impl TransferActor {
    pub(super) async fn offer_clip(&mut self, peers: &[DeviceId], clip: LocalClip) -> Result<(), Error> {
        let (mimes, content, hash) = clip.content()?;
        if content.size() > MAX_CLIP_SIZE {
            return Err(Error::Unexpected("a clip over 64 MiB"));
        }
        if self.clips.applied.as_ref() == Some(&hash) {
            log::debug!("the clip came from a peer; not offering it back");
            return Ok(());
        }
        let inline = match &content {
            Content::Text(text) if text.len() <= MAX_SHARE_LEN && mimes.iter().any(|mime| is_text_mime(mime)) => {
                Some(text.to_string())
            }
            _ => None,
        };
        let size = content.size();
        self.clips.local = Some(content);
        for peer in peers {
            let offer = self.clips.peers.entry(peer.clone()).or_default().offer(mimes.clone(), size, inline.clone());
            if !offer.is_valid() {
                return Err(Error::Unexpected("a clip offer that breaks the rules"));
            }
            self.send_to(peer, Message::ClipOffer(offer)).await;
        }
        Ok(())
    }

    pub(super) fn on_clip_offer(&mut self, peer: &DeviceId, offer: ClipOffer) {
        if let Some(text) = &offer.text {
            self.clips.applied = Some(digest::digest(&digest::SHA256, text.as_bytes()).as_ref().to_vec());
        }
        let event = TransferEvent::ClipOffered {
            from: peer.clone(),
            id: offer.id,
            mimes: offer.mimes.clone(),
            size: offer.size,
            text: offer.text.clone(),
        };
        self.clips.peers.entry(peer.clone()).or_default().on_offer(offer);
        self.emit(event);
    }

    /// Answers a pull on a new stream; a stale one gets a stream that is reset at once.
    pub(super) fn on_clip_pull(&mut self, peer: &DeviceId, pull: &ClipPull) {
        let Some(session) = self.sessions.get(peer).filter(|session| session.is_live()).cloned() else { return };
        let served =
            self.clips.peers.get(peer).map_or(Err(link_proto::clip::ClipRefused::Stale), |state| state.on_pull(pull));
        let content = match served {
            Ok(Serve::Text(text)) => Some(Content::Text(text.into())),
            Ok(Serve::Stream { .. }) => self.clips.local.clone(),
            Err(refused) => {
                log::info!("{peer}: clipboard pull refused: {refused}");
                None
            }
        };
        let (peer, header) = (peer.clone(), pull.clone());
        self.tasks.spawn(async move {
            let result = serve(&session, header, content).await;
            TaskEnd::ClipServed { peer, result }
        });
    }

    pub(super) async fn pull_clip(
        &mut self,
        peer: DeviceId,
        id: u64,
        mime: String,
        sink: File,
        reply: oneshot::Sender<Result<u64, Error>>,
    ) {
        let started = self
            .clips
            .peers
            .get_mut(&peer)
            .map_or(Err(link_proto::clip::ClipRefused::Stale), |state| state.pull(id, &mime));
        match started {
            Ok(Serve::Text(text)) => {
                self.tasks.spawn(async move {
                    let written = write_sink(sink, text.as_bytes()).await.map(|()| text.len() as u64);
                    drop(reply.send(written));
                    TaskEnd::ClipServed { peer, result: Ok(()) }
                });
            }
            Ok(Serve::Stream { .. }) => {
                let header = ClipPull { id, mime };
                if !self.send_to(&peer, Message::ClipPull(header.clone())).await {
                    drop(reply.send(Err(Error::NotConnected)));
                    return;
                }
                let deadline = Instant::now() + STEP_TIMEOUT;
                self.clips.pulls.push(Pull { peer, header, sink: Some(sink), reply, deadline });
            }
            Err(refused) => drop(reply.send(Err(Error::ClipRefused(refused)))),
        }
    }

    pub(super) fn on_clip_data(&mut self, peer: &DeviceId, mut recv: RecvStream, header: ClipPull) {
        let pending = self.clips.pulls.iter_mut().find(|pull| pull.peer == *peer && pull.header == header);
        let admitted = self.clips.peers.get_mut(peer).map(|state| state.on_data(&header));
        let (Some(pull), Some(Ok(budget))) = (pending, admitted) else {
            log::warn!("{peer}: a clip-data stream nobody pulled");
            recv.stop(PROTOCOL_ERROR);
            return;
        };
        let Some(sink) = pull.sink.take() else { return };
        let peer = peer.clone();
        self.tasks.spawn(async move {
            let result = copy(recv, sink, budget).await;
            TaskEnd::ClipPulled { peer, header, result }
        });
    }

    pub(super) fn clip_pulled(&mut self, peer: &DeviceId, header: &ClipPull, result: Result<(u64, Vec<u8>), Error>) {
        let Some(index) = self.clips.pulls.iter().position(|pull| pull.peer == *peer && pull.header == *header) else {
            return;
        };
        let pull = self.clips.pulls.remove(index);
        let result = result.map(|(bytes, hash)| {
            self.clips.applied = Some(hash);
            bytes
        });
        drop(pull.reply.send(result));
    }

    pub(super) fn clip_deadlines(&mut self) {
        let now = Instant::now();
        let (expired, waiting): (Vec<Pull>, Vec<Pull>) =
            self.clips.pulls.drain(..).partition(|pull| pull.sink.is_some() && pull.deadline <= now);
        self.clips.pulls = waiting;
        for pull in expired {
            drop(pull.reply.send(Err(Error::Timeout)));
        }
    }
}

async fn serve(session: &SessionHandle, header: ClipPull, content: Option<Content>) -> Result<(), Error> {
    let stream = session.connection().open_uni().await?;
    let mut out = Outbound { stream, finished: false };
    write_frame(&mut out.stream, Message::ClipData(header), session.tap()).await?;
    match content {
        None => {
            out.stream.reset(CANCELLED);
            out.finished = true;
            return Ok(());
        }
        Some(Content::Text(text)) => out.stream.write_all(text.as_bytes()).await?,
        Some(Content::File(file, size)) => {
            let mut buf = vec![0; CHUNK];
            let mut offset = 0;
            while offset < size {
                let want = usize::try_from(size - offset).map_or(CHUNK, |left| left.min(CHUNK));
                let read = file.read_at(&mut buf[..want], offset)?;
                if read == 0 {
                    break;
                }
                out.stream.write_all(&buf[..read]).await?;
                offset += read as u64;
            }
        }
    }
    out.stream.finish()?;
    out.finished = true;
    Ok(())
}

/// A paste target: a pipe (a Wayland data source's) is written without blocking the runtime, a file directly.
enum Sink {
    Pipe(pipe::Sender),
    File(File),
}

impl Sink {
    fn new(file: File) -> Result<Self, Error> {
        if file.metadata()?.file_type().is_fifo() {
            return Ok(Self::Pipe(pipe::Sender::from_file(file)?));
        }
        Ok(Self::File(file))
    }

    async fn write_all(&mut self, bytes: &[u8]) -> Result<(), Error> {
        match self {
            Self::Pipe(pipe) => pipe.write_all(bytes).await?,
            Self::File(file) => file.write_all(bytes)?,
        }
        Ok(())
    }
}

async fn write_sink(sink: File, bytes: &[u8]) -> Result<(), Error> {
    Sink::new(sink)?.write_all(bytes).await
}

/// Copies a `clip-data` stream into the sink within its budget, returning the byte count and hash.
async fn copy(mut recv: RecvStream, sink: File, mut budget: Budget) -> Result<(u64, Vec<u8>), Error> {
    let mut sink = Sink::new(sink)?;
    let mut context = digest::Context::new(&digest::SHA256);
    loop {
        let chunk = tokio::time::timeout(STEP_TIMEOUT, recv.read_chunk(CHUNK)).await?;
        let Some(chunk) = chunk.map_err(|_| Error::StreamEnded)? else { break };
        if budget.take(chunk.len()).is_err() {
            recv.stop(PROTOCOL_ERROR);
            return Err(Error::Unexpected("clipboard bytes past the offered size"));
        }
        sink.write_all(&chunk).await?;
        context.update(&chunk);
    }
    Ok((budget.offset(), context.finish().as_ref().to_vec()))
}
