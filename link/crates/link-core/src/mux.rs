//! QUIC's stream model over one ordered byte stream, for Bluetooth: a reader task and a writer task per connection,
//! [`link_proto::mux`] frames between them, a credit window per stream, and keep-alive with the same idle timeout as
//! QUIC. The API mirrors the part of quinn the session and transfers use, so [`crate::wire`] can hold either.

use std::collections::HashMap;
use std::sync::{Arc, Mutex, PoisonError};
use std::time::Duration;

use link_proto::mux::{self, Frame, Header, MuxError};
use tokio::io::{AsyncRead, AsyncReadExt, AsyncWrite, AsyncWriteExt};
use tokio::sync::{Notify, Semaphore, mpsc, watch};

/// No frame for this long ends the connection, as QUIC's idle timeout does.
pub const IDLE_TIMEOUT: Duration = Duration::from_secs(30);
/// Bulk bytes queued for the writer before `write_all` waits, so a fast file never buffers without bound.
const QUEUED: usize = 4 * mux::MAX_DATA;

/// How a connection ended.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Closed {
    /// We closed it.
    Local,
    /// The peer closed it with an application close code.
    Peer {
        code: u32,
        reason: String,
    },
    TimedOut,
    /// The peer broke the framing or a stream rule; we closed with the protocol-error code.
    Violation(MuxError),
    /// The byte stream failed or ended without a close.
    Lost(String),
}

#[derive(Debug, Clone, PartialEq, Eq, thiserror::Error)]
pub enum StreamError {
    #[error("the peer stopped the stream with {0}")]
    Stopped(u32),
    #[error("the peer reset the stream with {0}")]
    Reset(u32),
    #[error("the stream is already finished")]
    Finished,
    #[error("the connection ended: {0:?}")]
    ConnectionLost(Closed),
}

enum Incoming {
    Data(Vec<u8>),
    Fin,
    Reset(u32),
}

struct SendSide {
    window: Semaphore,
    stopped: Mutex<Option<u32>>,
    stop: Notify,
}

struct Slot {
    recv: Option<mpsc::UnboundedSender<Incoming>>,
    /// Bytes the peer may still send on this stream before it must wait for credit.
    recv_budget: u32,
    send: Option<Arc<SendSide>>,
}

struct State {
    next: u32,
    /// The highest stream id the peer opened, so late frames for a stream already gone are told from bad ids.
    peer_high: Option<u32>,
    streams: HashMap<u32, Slot>,
}

impl State {
    fn peer_open(&self, client: bool) -> usize {
        self.streams.keys().filter(|&&id| mux::opened_by_client(id) != client).count()
    }
}

struct Inner {
    client: bool,
    urgent: mpsc::UnboundedSender<Vec<u8>>,
    bulk: mpsc::UnboundedSender<Vec<u8>>,
    queued: Arc<Semaphore>,
    state: Mutex<State>,
    closed: watch::Sender<Option<Closed>>,
    accept_bi: tokio::sync::Mutex<mpsc::UnboundedReceiver<(MuxSend, MuxRecv)>>,
    accept_uni: tokio::sync::Mutex<mpsc::UnboundedReceiver<MuxRecv>>,
}

impl Inner {
    fn state(&self) -> std::sync::MutexGuard<'_, State> {
        self.state.lock().unwrap_or_else(PoisonError::into_inner)
    }

    fn reason(&self) -> Option<Closed> {
        self.closed.borrow().clone()
    }

    /// Records how the connection ended, once; later causes are the first one's consequences.
    fn end(&self, closed: Closed) {
        self.closed.send_if_modified(|current| {
            if current.is_some() {
                return false;
            }
            *current = Some(closed);
            true
        });
        let mut state = self.state();
        for slot in state.streams.values() {
            if let Some(send) = &slot.send {
                send.window.close();
                send.stop.notify_waiters();
            }
        }
        state.streams.clear();
    }

    fn lost(&self) -> StreamError {
        StreamError::ConnectionLost(self.reason().unwrap_or(Closed::Local))
    }
}

/// One connection. Cheap to clone; all clones are the same connection, and dropping the last one closes it with 0,
/// as quinn does. Streams do not keep it open.
#[derive(Clone)]
pub struct MuxConnection {
    inner: Arc<Inner>,
    _last: Arc<CloseOnDrop>,
}

struct CloseOnDrop(Arc<Inner>);

impl Drop for CloseOnDrop {
    fn drop(&mut self) {
        close(&self.0, 0, "");
    }
}

impl std::fmt::Debug for MuxConnection {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("MuxConnection").field("client", &self.inner.client).finish_non_exhaustive()
    }
}

impl MuxConnection {
    /// Starts the reader and writer tasks over `io`, which is already authenticated (TLS). The client is the phone;
    /// with `keep_alive` it pings that often, as QUIC keep-alive does.
    pub fn new<T>(io: T, client: bool, keep_alive: Option<Duration>) -> Self
    where
        T: AsyncRead + AsyncWrite + Send + 'static,
    {
        let (read, write) = tokio::io::split(io);
        let (urgent, urgent_rx) = mpsc::unbounded_channel();
        let (bulk, bulk_rx) = mpsc::unbounded_channel();
        let (bi_tx, bi_rx) = mpsc::unbounded_channel();
        let (uni_tx, uni_rx) = mpsc::unbounded_channel();
        let queued = Arc::new(Semaphore::new(QUEUED));
        let inner = Arc::new(Inner {
            client,
            urgent,
            bulk,
            queued: queued.clone(),
            state: Mutex::new(State { next: u32::from(!client), peer_high: None, streams: HashMap::new() }),
            closed: watch::channel(None).0,
            accept_bi: tokio::sync::Mutex::new(bi_rx),
            accept_uni: tokio::sync::Mutex::new(uni_rx),
        });
        tokio::spawn(write_loop(write, urgent_rx, bulk_rx, queued, inner.clone()));
        tokio::spawn(read_loop(read, Accept { bi: bi_tx, uni: uni_tx }, inner.clone()));
        if let Some(every) = keep_alive {
            tokio::spawn(ping_loop(every, Arc::downgrade(&inner)));
        }
        Self { _last: Arc::new(CloseOnDrop(inner.clone())), inner }
    }

    pub fn open_bi(&self) -> Result<(MuxSend, MuxRecv), StreamError> {
        let id = self.open(true)?;
        Ok((
            MuxSend::new(id, self.side(id)?, self.inner.clone(), true),
            MuxRecv::new(id, self.receiver(id)?, self.inner.clone()),
        ))
    }

    pub fn open_uni(&self) -> Result<MuxSend, StreamError> {
        let id = self.open(false)?;
        Ok(MuxSend::new(id, self.side(id)?, self.inner.clone(), false))
    }

    pub async fn accept_bi(&self) -> Result<(MuxSend, MuxRecv), StreamError> {
        let mut queue = self.inner.accept_bi.lock().await;
        tokio::select! {
            pair = queue.recv() => pair.ok_or_else(|| self.inner.lost()),
            closed = self.closed() => Err(StreamError::ConnectionLost(closed)),
        }
    }

    pub async fn accept_uni(&self) -> Result<MuxRecv, StreamError> {
        let mut queue = self.inner.accept_uni.lock().await;
        tokio::select! {
            recv = queue.recv() => recv.ok_or_else(|| self.inner.lost()),
            closed = self.closed() => Err(StreamError::ConnectionLost(closed)),
        }
    }

    /// Sends CLOSE with `code` and ends every stream; the writer closes the byte stream once CLOSE is out.
    pub fn close(&self, code: u32, reason: &str) {
        close(&self.inner, code, reason);
    }

    pub fn close_reason(&self) -> Option<Closed> {
        self.inner.reason()
    }

    pub async fn closed(&self) -> Closed {
        let mut watch = self.inner.closed.subscribe();
        match watch.wait_for(Option::is_some).await {
            Ok(closed) => closed.clone().unwrap_or(Closed::Local),
            Err(_) => Closed::Local,
        }
    }

    /// Unique among live connections, as quinn's is.
    pub fn stable_id(&self) -> usize {
        Arc::as_ptr(&self.inner) as usize
    }

    fn open(&self, bidi: bool) -> Result<u32, StreamError> {
        if self.inner.reason().is_some() {
            return Err(self.inner.lost());
        }
        let mut state = self.inner.state();
        let id = state.next;
        state.next += 2;
        state.streams.insert(id, Slot { recv: None, recv_budget: 0, send: Some(new_side()) });
        drop(state);
        let frame = if bidi { Frame::OpenBi { stream: id } } else { Frame::OpenUni { stream: id } };
        let channel = if bidi { &self.inner.urgent } else { &self.inner.bulk };
        channel.send(frame.encode()).map_err(|_| self.inner.lost())?;
        Ok(id)
    }

    fn side(&self, id: u32) -> Result<Arc<SendSide>, StreamError> {
        self.inner.state().streams.get(&id).and_then(|slot| slot.send.clone()).ok_or_else(|| self.inner.lost())
    }

    /// The receive half of a bidirectional stream we opened.
    fn receiver(&self, id: u32) -> Result<mpsc::UnboundedReceiver<Incoming>, StreamError> {
        let (tx, rx) = mpsc::unbounded_channel();
        let mut state = self.inner.state();
        let slot = state.streams.get_mut(&id).ok_or_else(|| self.inner.lost())?;
        slot.recv = Some(tx);
        slot.recv_budget = mux::WINDOW;
        Ok(rx)
    }
}

fn close(inner: &Inner, code: u32, reason: &str) {
    if inner.reason().is_some() {
        return;
    }
    drop(inner.urgent.send(Frame::Close { code, reason: reason.to_owned() }.encode()));
    inner.end(Closed::Local);
}

fn new_side() -> Arc<SendSide> {
    Arc::new(SendSide { window: Semaphore::new(mux::WINDOW as usize), stopped: Mutex::new(None), stop: Notify::new() })
}

/// Writes one stream. Dropped unfinished, it finishes the stream, as quinn does.
pub struct MuxSend {
    id: u32,
    side: Arc<SendSide>,
    inner: Arc<Inner>,
    /// The control stream's frames go ahead of bulk data.
    urgent: bool,
    done: bool,
}

impl MuxSend {
    fn new(id: u32, side: Arc<SendSide>, inner: Arc<Inner>, urgent: bool) -> Self {
        Self { id, side, inner, urgent, done: false }
    }

    pub async fn write_all(&mut self, bytes: &[u8]) -> Result<(), StreamError> {
        if self.done {
            return Err(StreamError::Finished);
        }
        for chunk in bytes.chunks(mux::MAX_DATA) {
            let len = u32::try_from(chunk.len()).unwrap_or(u32::MAX);
            match self.side.window.acquire_many(len).await {
                Ok(permit) => permit.forget(),
                Err(_) => return Err(self.why_closed()),
            }
            if !self.urgent {
                match self.inner.queued.clone().acquire_many_owned(len).await {
                    Ok(permit) => permit.forget(),
                    Err(_) => return Err(self.inner.lost()),
                }
            }
            self.send(&Frame::Data { stream: self.id, bytes: chunk.to_vec() })?;
        }
        Ok(())
    }

    pub fn finish(&mut self) -> Result<(), StreamError> {
        if self.done {
            return Err(StreamError::Finished);
        }
        self.done = true;
        self.send(&Frame::Fin { stream: self.id })?;
        self.forget_send();
        Ok(())
    }

    /// Abandons the stream; the peer drops what it received of it.
    pub fn reset(&mut self, code: u32) -> Result<(), StreamError> {
        if self.done {
            return Err(StreamError::Finished);
        }
        self.done = true;
        self.send(&Frame::Reset { stream: self.id, code })?;
        self.forget_send();
        Ok(())
    }

    /// Waits until the peer stops the stream (its code) or the connection ends (None).
    pub async fn stopped(&mut self) -> Option<u32> {
        loop {
            let notified = self.side.stop.notified();
            if let Some(code) = *self.side.stopped.lock().unwrap_or_else(PoisonError::into_inner) {
                return Some(code);
            }
            if self.inner.reason().is_some() {
                return None;
            }
            notified.await;
        }
    }

    fn send(&self, frame: &Frame) -> Result<(), StreamError> {
        let channel = if self.urgent { &self.inner.urgent } else { &self.inner.bulk };
        channel.send(frame.encode()).map_err(|_| self.inner.lost())
    }

    fn why_closed(&self) -> StreamError {
        match *self.side.stopped.lock().unwrap_or_else(PoisonError::into_inner) {
            Some(code) => StreamError::Stopped(code),
            None => self.inner.lost(),
        }
    }

    /// Our half is over; the slot stays while a receive half of the same stream still reads.
    fn forget_send(&self) {
        let mut state = self.inner.state();
        if let Some(slot) = state.streams.get_mut(&self.id) {
            slot.send = None;
            if slot.recv.is_none() {
                state.streams.remove(&self.id);
            }
        }
    }
}

impl Drop for MuxSend {
    fn drop(&mut self) {
        if !self.done {
            drop(self.finish());
        }
    }
}

/// Reads one stream. Dropped before its end, it stops the stream with code 0, as quinn does.
pub struct MuxRecv {
    id: u32,
    rx: mpsc::UnboundedReceiver<Incoming>,
    inner: Arc<Inner>,
    pending: Vec<u8>,
    at: usize,
    /// Bytes read but not yet granted back to the peer.
    unacked: u32,
    done: bool,
}

impl MuxRecv {
    fn new(id: u32, rx: mpsc::UnboundedReceiver<Incoming>, inner: Arc<Inner>) -> Self {
        Self { id, rx, inner, pending: Vec::new(), at: 0, unacked: 0, done: false }
    }

    /// Up to `max` bytes, or None at the end of the stream.
    pub async fn read_chunk(&mut self, max: usize) -> Result<Option<Vec<u8>>, StreamError> {
        if self.at == self.pending.len() {
            if self.done {
                return Ok(None);
            }
            match self.rx.recv().await {
                Some(Incoming::Data(bytes)) => {
                    self.pending = bytes;
                    self.at = 0;
                }
                Some(Incoming::Fin) => {
                    self.done = true;
                    return Ok(None);
                }
                Some(Incoming::Reset(code)) => {
                    self.done = true;
                    return Err(StreamError::Reset(code));
                }
                None => {
                    self.done = true;
                    return Err(self.inner.lost());
                }
            }
        }
        let end = self.pending.len().min(self.at + max.max(1));
        let chunk = self.pending[self.at..end].to_vec();
        self.at = end;
        self.grant(chunk.len());
        Ok(Some(chunk))
    }

    pub async fn read_exact(&mut self, buf: &mut [u8]) -> Result<(), StreamError> {
        let mut filled = 0;
        while filled < buf.len() {
            let Some(chunk) = self.read_chunk(buf.len() - filled).await? else {
                return Err(StreamError::Finished);
            };
            buf[filled..filled + chunk.len()].copy_from_slice(&chunk);
            filled += chunk.len();
        }
        Ok(())
    }

    /// Tells the peer to stop sending; what is in flight is dropped on arrival.
    pub fn stop(&mut self, code: u32) {
        if std::mem::replace(&mut self.done, true) {
            return;
        }
        drop(self.inner.urgent.send(Frame::Stop { stream: self.id, code }.encode()));
        let mut state = self.inner.state();
        if let Some(slot) = state.streams.get_mut(&self.id) {
            slot.recv = None;
            if slot.send.is_none() {
                state.streams.remove(&self.id);
            }
        }
    }

    /// Grants the peer window back once a quarter of it is read, so credit frames stay rare.
    fn grant(&mut self, read: usize) {
        self.unacked += u32::try_from(read).unwrap_or(u32::MAX);
        if self.unacked < mux::WINDOW / 4 {
            return;
        }
        let bytes = std::mem::take(&mut self.unacked);
        if let Some(slot) = self.inner.state().streams.get_mut(&self.id) {
            slot.recv_budget += bytes;
        }
        drop(self.inner.urgent.send(Frame::Credit { stream: self.id, bytes }.encode()));
    }
}

impl Drop for MuxRecv {
    fn drop(&mut self) {
        if !self.done {
            self.stop(0);
        }
    }
}

struct Accept {
    bi: mpsc::UnboundedSender<(MuxSend, MuxRecv)>,
    uni: mpsc::UnboundedSender<MuxRecv>,
}

async fn read_loop<R: AsyncRead + Unpin>(mut read: R, accept: Accept, inner: Arc<Inner>) {
    let mut closed = inner.closed.subscribe();
    let ended = loop {
        let frame = tokio::select! {
            frame = tokio::time::timeout(IDLE_TIMEOUT, read_frame(&mut read)) => frame,
            _ = closed.wait_for(Option::is_some) => return,
        };
        let frame = match frame {
            Err(_) => break Closed::TimedOut,
            Ok(Err(ReadFailure::Io(error))) => break Closed::Lost(error.to_string()),
            Ok(Err(ReadFailure::Mux(error))) => break violation(&inner, error),
            Ok(Ok(frame)) => frame,
        };
        if let Err(error) = on_frame(frame, &accept, &inner) {
            break match error {
                Handled::Violation(error) => violation(&inner, error),
                Handled::Closed(closed) => closed,
            };
        }
    };
    inner.end(ended);
}

enum ReadFailure {
    Io(std::io::Error),
    Mux(MuxError),
}

async fn read_frame<R: AsyncRead + Unpin>(read: &mut R) -> Result<Frame, ReadFailure> {
    let mut header = [0; mux::HEADER_LEN];
    read.read_exact(&mut header).await.map_err(ReadFailure::Io)?;
    let header = Header::parse(header).map_err(ReadFailure::Mux)?;
    let mut payload = vec![0; header.len];
    read.read_exact(&mut payload).await.map_err(ReadFailure::Io)?;
    Frame::decode(header, payload).map_err(ReadFailure::Mux)
}

/// Closes with the protocol-error code, as a QUIC peer would for a stream-rule violation.
fn violation(inner: &Inner, error: MuxError) -> Closed {
    let code = link_proto::CloseCode::ProtocolError;
    drop(inner.urgent.send(Frame::Close { code: code as u32, reason: code.reason().to_owned() }.encode()));
    Closed::Violation(error)
}

enum Handled {
    Violation(MuxError),
    Closed(Closed),
}

fn on_frame(frame: Frame, accept: &Accept, inner: &Arc<Inner>) -> Result<(), Handled> {
    let bad = |kind| Handled::Violation(MuxError::Malformed(kind));
    match frame {
        Frame::OpenBi { stream } | Frame::OpenUni { stream } => {
            let bidi = matches!(frame, Frame::OpenBi { .. });
            let kind = if bidi { mux::Kind::OpenBi } else { mux::Kind::OpenUni };
            let mut state = inner.state();
            let fresh = state.peer_high.is_none_or(|high| stream > high);
            if mux::opened_by_client(stream) == inner.client
                || !fresh
                || state.peer_open(inner.client) >= mux::MAX_PEER_STREAMS
            {
                return Err(bad(kind));
            }
            state.peer_high = Some(stream);
            let (tx, rx) = mpsc::unbounded_channel();
            let side = bidi.then(new_side);
            state.streams.insert(stream, Slot { recv: Some(tx), recv_budget: mux::WINDOW, send: side.clone() });
            drop(state);
            let recv = MuxRecv::new(stream, rx, inner.clone());
            // A closed accept queue means nobody takes streams any more; dropping the halves stops them.
            if let Some(side) = side {
                drop(accept.bi.send((MuxSend::new(stream, side, inner.clone(), true), recv)));
            } else {
                drop(accept.uni.send(recv));
            }
        }
        Frame::Data { stream, bytes } => {
            let mut state = inner.state();
            let known = known(&state, stream, inner.client);
            match state.streams.get_mut(&stream) {
                Some(slot) if slot.recv.is_some() => {
                    let len = u32::try_from(bytes.len()).unwrap_or(u32::MAX);
                    if len > slot.recv_budget {
                        return Err(Handled::Violation(MuxError::TooLong { kind: mux::Kind::Data, len: bytes.len() }));
                    }
                    slot.recv_budget -= len;
                    if let Some(tx) = &slot.recv {
                        drop(tx.send(Incoming::Data(bytes)));
                    }
                }
                // Bytes in flight when we stopped the stream.
                _ if known => {}
                _ => return Err(bad(mux::Kind::Data)),
            }
        }
        Frame::Fin { stream } | Frame::Reset { stream, .. } => {
            let incoming = match frame {
                Frame::Reset { code, .. } => Incoming::Reset(code),
                _ => Incoming::Fin,
            };
            let mut state = inner.state();
            let known = known(&state, stream, inner.client);
            match state.streams.get_mut(&stream) {
                Some(slot) if slot.recv.is_some() => {
                    if let Some(tx) = slot.recv.take() {
                        drop(tx.send(incoming));
                    }
                    if slot.send.is_none() {
                        state.streams.remove(&stream);
                    }
                }
                _ if known => {}
                _ => return Err(bad(mux::Kind::Fin)),
            }
        }
        Frame::Stop { stream, code } => {
            let state = inner.state();
            if let Some(side) = state.streams.get(&stream).and_then(|slot| slot.send.clone()) {
                *side.stopped.lock().unwrap_or_else(PoisonError::into_inner) = Some(code);
                side.window.close();
                side.stop.notify_waiters();
            } else if !known(&state, stream, inner.client) {
                return Err(bad(mux::Kind::Stop));
            }
        }
        Frame::Credit { stream, bytes } => {
            let state = inner.state();
            if let Some(side) = state.streams.get(&stream).and_then(|slot| slot.send.clone()) {
                let open = side.window.available_permits();
                if open + bytes as usize > Semaphore::MAX_PERMITS.min(u32::MAX as usize) {
                    return Err(bad(mux::Kind::Credit));
                }
                side.window.add_permits(bytes as usize);
            } else if !known(&state, stream, inner.client) {
                return Err(bad(mux::Kind::Credit));
            }
        }
        Frame::Close { code, reason } => return Err(Handled::Closed(Closed::Peer { code, reason })),
        Frame::Ping => drop(inner.urgent.send(Frame::Pong.encode())),
        Frame::Pong => {}
    }
    Ok(())
}

/// Whether `stream` existed on this connection, so a frame for it now gone is late rather than invalid.
fn known(state: &State, stream: u32, client: bool) -> bool {
    if mux::opened_by_client(stream) == client {
        stream < state.next
    } else {
        state.peer_high.is_some_and(|high| stream <= high)
    }
}

async fn write_loop<W: AsyncWrite + Unpin>(
    mut write: W,
    mut urgent: mpsc::UnboundedReceiver<Vec<u8>>,
    mut bulk: mpsc::UnboundedReceiver<Vec<u8>>,
    queued: Arc<Semaphore>,
    inner: Arc<Inner>,
) {
    let mut closed = inner.closed.subscribe();
    loop {
        let (frame, from_bulk) = tokio::select! {
            biased;
            Some(frame) = urgent.recv() => (frame, false),
            Some(frame) = bulk.recv() => (frame, true),
            _ = closed.wait_for(Option::is_some) => break,
        };
        if let Err(error) = write.write_all(&frame).await {
            inner.end(Closed::Lost(error.to_string()));
            return;
        }
        if from_bulk {
            queued.add_permits(data_len(&frame));
        }
        if urgent.is_empty() && bulk.is_empty() && write.flush().await.is_err() {
            inner.end(Closed::Lost("flush failed".to_owned()));
            return;
        }
    }
    // Closed: what is already queued for the peer, CLOSE last among it, goes out before the byte stream ends.
    while let Ok(frame) = urgent.try_recv() {
        if write.write_all(&frame).await.is_err() {
            break;
        }
    }
    drop(write.flush().await);
    drop(write.shutdown().await);
}

/// The permits a queued DATA frame holds; every other bulk frame holds none.
fn data_len(frame: &[u8]) -> usize {
    if frame.first() == Some(&(mux::Kind::Data as u8)) { frame.len() - mux::HEADER_LEN } else { 0 }
}

async fn ping_loop(every: Duration, inner: std::sync::Weak<Inner>) {
    let mut tick = tokio::time::interval(every);
    tick.tick().await;
    loop {
        tick.tick().await;
        let Some(inner) = inner.upgrade() else { return };
        if inner.reason().is_some() || inner.urgent.send(Frame::Ping.encode()).is_err() {
            return;
        }
    }
}
