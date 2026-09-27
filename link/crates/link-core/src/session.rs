//! The session actor: one per connection, on both sides, after both hellos. It owns the control stream, delivers what
//! the peer shares, hands transfer messages and bulk streams to the transfer actor, and sends what its handle asks for.

use std::collections::HashMap;
use std::pin::pin;

use link_proto::CloseCode;
use link_proto::limit::Limits;
use link_proto::message::{Envelope, Message, OfferReply, RefuseReason, Share, ShareAck, Status};
use link_proto::session::{Inbound, Role, SessionState};
use tokio::sync::{mpsc, oneshot};

use crate::control::{Control, ControlReader, ControlWriter, STEP_TIMEOUT, Tap};
use crate::identity::DeviceId;
use crate::transfer::TransferHandle;
use crate::{Error, close};

/// What a session hands to its owner.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum SessionEvent {
    Received {
        from: DeviceId,
        share: Share,
    },
    /// The phone unpaired itself; only a desktop's sessions report this.
    Unpaired {
        from: DeviceId,
    },
    /// The phone's battery and network; only a desktop's sessions report this.
    Status {
        from: DeviceId,
        status: Status,
    },
}

/// Where a session delivers what its peer sends.
pub struct Route {
    pub peer: DeviceId,
    pub events: mpsc::Sender<SessionEvent>,
    pub transfers: TransferHandle,
}

enum Command {
    Share { share: Share, reply: oneshot::Sender<Result<(), Error>> },
    Unpair { reply: oneshot::Sender<Result<(), Error>> },
    Send { message: Message, reply: oneshot::Sender<Result<(), Error>> },
}

/// Reaches a running session. Cheap to clone; every clone stops working when the session ends.
#[derive(Clone)]
pub struct SessionHandle {
    commands: mpsc::Sender<Command>,
    connection: quinn::Connection,
    tap: Option<Tap>,
}

pub struct SessionActor {
    reader: ControlReader,
    commands: mpsc::Receiver<Command>,
    live: Live,
}

struct Live {
    connection: quinn::Connection,
    writer: ControlWriter,
    state: SessionState,
    /// Applied to what a phone sends a desktop.
    limits: Option<Limits>,
    route: Route,
    waiting: HashMap<u64, oneshot::Sender<Result<(), Error>>>,
}

/// Spawns nothing: the caller runs the actor in a task it owns and keeps the handle.
pub fn session(
    connection: quinn::Connection,
    control: Control,
    role: Role,
    route: Route,
) -> (SessionHandle, SessionActor) {
    let (commands_tx, commands) = mpsc::channel(8);
    let handle = SessionHandle { commands: commands_tx, connection: connection.clone(), tap: control.tap() };
    let (reader, writer) = control.split();
    let limits = (role == Role::Desktop).then(Limits::default);
    let live = Live { connection, writer, state: SessionState::new(role), limits, route, waiting: HashMap::new() };
    (handle, SessionActor { reader, commands, live })
}

impl SessionHandle {
    /// Sends `share` and waits up to [`STEP_TIMEOUT`] for the peer's ack.
    pub async fn share(&self, share: Share) -> Result<(), Error> {
        share.check()?;
        let (reply, acked) = oneshot::channel();
        self.commands.send(Command::Share { share, reply }).await.map_err(|_| Error::NotConnected)?;
        tokio::time::timeout(STEP_TIMEOUT, acked).await?.map_err(|_| Error::NotConnected)?
    }

    /// Sends a message that expects no ack.
    pub async fn send(&self, message: Message) -> Result<(), Error> {
        let (reply, sent) = oneshot::channel();
        self.commands.send(Command::Send { message, reply }).await.map_err(|_| Error::NotConnected)?;
        sent.await.map_err(|_| Error::NotConnected)?
    }

    /// Tells the desktop this phone unpaired and waits for it to close the connection.
    pub async fn unpair(&self) -> Result<(), Error> {
        let (reply, sent) = oneshot::channel();
        self.commands.send(Command::Unpair { reply }).await.map_err(|_| Error::NotConnected)?;
        sent.await.map_err(|_| Error::NotConnected)??;
        tokio::time::timeout(STEP_TIMEOUT, self.connection.closed()).await?;
        Ok(())
    }

    pub fn close(&self, code: CloseCode) {
        close(&self.connection, code);
    }

    pub fn is_live(&self) -> bool {
        self.connection.close_reason().is_none()
    }

    pub fn stable_id(&self) -> usize {
        self.connection.stable_id()
    }

    pub fn connection(&self) -> &quinn::Connection {
        &self.connection
    }

    /// The transcript tap of this session's control stream, which bulk stream headers go through too.
    pub fn tap(&self) -> Option<&Tap> {
        self.tap.as_ref()
    }
}

impl SessionActor {
    /// Runs until the connection ends. The error says how; a protocol violation is the caller's to close with.
    pub async fn run(self) -> Result<(), Error> {
        let Self { reader, mut commands, mut live } = self;
        let (inbox_tx, mut inbox) = mpsc::channel(8);
        let mut reading = pin!(reader.pump(inbox_tx));
        let connection = live.connection.clone();
        loop {
            tokio::select! {
                ended = &mut reading => return ended.and(Err(Error::StreamEnded)),
                Some(envelope) = inbox.recv() => {
                    if live.on_envelope(envelope).await? {
                        close(&live.connection, CloseCode::Done);
                        return Ok(());
                    }
                }
                Some(command) = commands.recv() => live.on_command(command).await?,
                stream = connection.accept_uni() => {
                    if !live.route.transfers.stream(live.route.peer.clone(), stream?) {
                        return Err(Error::Flooded);
                    }
                }
            }
        }
    }
}

impl Live {
    /// Handles one message from the peer; true when the session is over.
    async fn on_envelope(&mut self, envelope: Envelope) -> Result<bool, Error> {
        let peer = &self.route.peer;
        let now = std::time::Instant::now();
        if let Some(limits) = &mut self.limits
            && !limits.admit(&envelope.message, now)
        {
            log::warn!("{peer}: dropping a {} over its rate limit", envelope.message.kind());
            if let Message::Offer(offer) = &envelope.message {
                let reply = OfferReply { transfer: offer.transfer, accepted: false, reason: Some(RefuseReason::Busy) };
                self.writer.send(Message::OfferReply(reply)).await?;
            }
            return Ok(false);
        }
        match self.state.on_message(envelope)? {
            Inbound::Share { id, share } => {
                log::info!("{peer}: received a {} share", share.kind.as_str());
                self.emit(SessionEvent::Received { from: peer.clone(), share }).await;
                self.writer.send(Message::ShareAck(ShareAck { of: id })).await?;
            }
            Inbound::Acked { of } => {
                if let Some(reply) = self.waiting.remove(&of) {
                    drop(reply.send(Ok(())));
                }
            }
            Inbound::Unpair => {
                self.emit(SessionEvent::Unpaired { from: peer.clone() }).await;
                return Ok(true);
            }
            Inbound::Status(status) => self.emit(SessionEvent::Status { from: peer.clone(), status }).await,
            Inbound::Transfer(message) => {
                if !self.route.transfers.control(peer.clone(), message) {
                    return Err(Error::Flooded);
                }
            }
        }
        Ok(false)
    }

    async fn on_command(&mut self, command: Command) -> Result<(), Error> {
        match command {
            Command::Share { share, reply } => {
                let id = self.writer.send(Message::Share(share)).await?;
                self.state.sent_share(id);
                self.waiting.insert(id, reply);
            }
            Command::Unpair { reply } => {
                self.writer.send(Message::Unpair).await?;
                drop(reply.send(Ok(())));
            }
            Command::Send { message, reply } => {
                self.writer.send(message).await?;
                drop(reply.send(Ok(())));
            }
        }
        Ok(())
    }

    async fn emit(&self, event: SessionEvent) {
        if self.route.events.send(event).await.is_err() {
            log::debug!("{}: nobody receives session events", self.route.peer);
        }
    }
}
