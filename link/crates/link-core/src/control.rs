//! The control stream: length-prefixed CBOR envelopes on the phone's first bidirectional stream.

use std::sync::Arc;
use std::time::Duration;

use link_proto::VERSION;
use link_proto::frame::{self, HEADER_LEN};
use link_proto::message::{Envelope, Hello, Message};

use crate::Error;

/// How long a peer may take for one handshake step before the attempt is abandoned.
pub const STEP_TIMEOUT: Duration = Duration::from_secs(10);

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Direction {
    Sent,
    Received,
}

/// Observes every CBOR envelope on a control stream; the E2E transcripts are written through it.
pub type Tap = Arc<dyn Fn(Direction, &[u8]) + Send + Sync>;

pub struct Control {
    send: quinn::SendStream,
    recv: quinn::RecvStream,
    next_id: u64,
    tap: Option<Tap>,
}

impl Control {
    pub async fn open(connection: &quinn::Connection, tap: Option<Tap>) -> Result<Self, Error> {
        let (send, recv) = connection.open_bi().await?;
        Ok(Self { send, recv, next_id: 0, tap })
    }

    pub async fn accept(connection: &quinn::Connection, tap: Option<Tap>) -> Result<Self, Error> {
        let (send, recv) = tokio::time::timeout(STEP_TIMEOUT, connection.accept_bi()).await??;
        Ok(Self { send, recv, next_id: 0, tap })
    }

    pub async fn send(&mut self, message: Message) -> Result<(), Error> {
        let envelope = Envelope::new(self.next_id, message);
        self.next_id += 1;
        let bytes = frame::encode(&envelope);
        self.observe(Direction::Sent, &bytes[HEADER_LEN..]);
        self.send.write_all(&bytes).await?;
        Ok(())
    }

    /// The next message, which must arrive within [`STEP_TIMEOUT`].
    pub async fn recv(&mut self) -> Result<Message, Error> {
        tokio::time::timeout(STEP_TIMEOUT, self.recv_idle()).await?
    }

    /// The next message, waiting as long as the connection lives.
    pub async fn recv_idle(&mut self) -> Result<Message, Error> {
        let mut header = [0; HEADER_LEN];
        self.recv.read_exact(&mut header).await?;
        let mut body = vec![0; frame::body_len(header)?];
        self.recv.read_exact(&mut body).await?;
        self.observe(Direction::Received, &body);
        Ok(Envelope::from_cbor(&body)?.message)
    }

    /// Sends our hello and returns the peer's; the phone speaks first.
    pub async fn hello_as_client(&mut self, own: Hello) -> Result<Hello, Error> {
        self.send(Message::Hello(own)).await?;
        expect_hello(self.recv().await?)
    }

    pub async fn hello_as_server(&mut self, own: Hello) -> Result<Hello, Error> {
        let peer = expect_hello(self.recv().await?)?;
        self.send(Message::Hello(own)).await?;
        Ok(peer)
    }

    fn observe(&self, direction: Direction, body: &[u8]) {
        if let Some(tap) = &self.tap {
            tap(direction, body);
        }
    }
}

fn expect_hello(message: Message) -> Result<Hello, Error> {
    let Message::Hello(hello) = message else {
        return Err(Error::Unexpected(message.kind()));
    };
    if hello.version != VERSION {
        return Err(Error::Version(hello.version));
    }
    Ok(hello)
}
