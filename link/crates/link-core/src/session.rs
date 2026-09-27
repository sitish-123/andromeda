//! The session actor: one per connection, on both sides, after both hellos. It owns the control stream, delivers what
//! the peer shares, and sends what its handle asks for.

use std::collections::HashMap;
use std::pin::pin;

use link_proto::CloseCode;
use link_proto::message::{Envelope, Message, Share, ShareAck};
use link_proto::session::{Inbound, Role, SessionState};
use tokio::sync::{mpsc, oneshot};

use crate::control::{Control, ControlReader, ControlWriter, STEP_TIMEOUT};
use crate::identity::DeviceId;
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
    /// A feature message that is not acknowledged, already checked for its direction.
    Message {
        from: DeviceId,
        message: Message,
    },
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
    peer: DeviceId,
    events: mpsc::Sender<SessionEvent>,
    waiting: HashMap<u64, oneshot::Sender<Result<(), Error>>>,
}

/// Spawns nothing: the caller runs the actor in a task it owns and keeps the handle.
pub fn session(
    connection: quinn::Connection,
    control: Control,
    role: Role,
    peer: DeviceId,
    events: mpsc::Sender<SessionEvent>,
) -> (SessionHandle, SessionActor) {
    let (commands_tx, commands) = mpsc::channel(8);
    let handle = SessionHandle { commands: commands_tx, connection: connection.clone() };
    let (reader, writer) = control.split();
    let live = Live { connection, writer, state: SessionState::new(role), peer, events, waiting: HashMap::new() };
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

    /// Sends a message that is not acknowledged, and returns once it is written.
    pub async fn send(&self, message: Message) -> Result<(), Error> {
        message.validate()?;
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
}

impl SessionActor {
    /// Runs until the connection ends. The error says how; a protocol violation is the caller's to close with.
    pub async fn run(self) -> Result<(), Error> {
        let Self { reader, mut commands, mut live } = self;
        let (inbox_tx, mut inbox) = mpsc::channel(8);
        let mut reading = pin!(reader.pump(inbox_tx));
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
            }
        }
    }
}

impl Live {
    /// Handles one message from the peer; true when the session is over.
    async fn on_envelope(&mut self, envelope: Envelope) -> Result<bool, Error> {
        match self.state.on_message(envelope)? {
            Inbound::Share { id, share } => {
                log::info!("{}: received a {} share", self.peer, share.kind.as_str());
                self.emit(SessionEvent::Received { from: self.peer.clone(), share }).await;
                self.writer.send(Message::ShareAck(ShareAck { of: id })).await?;
                Ok(false)
            }
            Inbound::Acked { of } => {
                if let Some(reply) = self.waiting.remove(&of) {
                    drop(reply.send(Ok(())));
                }
                Ok(false)
            }
            Inbound::Unpair => {
                self.emit(SessionEvent::Unpaired { from: self.peer.clone() }).await;
                Ok(true)
            }
            Inbound::Deliver(message) => {
                self.emit(SessionEvent::Message { from: self.peer.clone(), message }).await;
                Ok(false)
            }
        }
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
        if self.events.send(event).await.is_err() {
            log::debug!("{}: nobody receives session events", self.peer);
        }
    }
}
