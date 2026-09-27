//! The file transfer rules of `link/ARCHITECTURE.md` (Files): which answers, streams, and bytes each side may accept.

use crate::message::{FileData, FileDone, FileOffset, Offer, OfferReply, RefuseReason, ResumeAt, TransferId};

/// Files a sender streams at once.
pub const MAX_IN_FLIGHT: usize = 4;

#[derive(Debug, Clone, PartialEq, Eq, thiserror::Error)]
pub enum TransferError {
    #[error("offer-reply for transfer {0}, which was not offered or was already answered")]
    UnexpectedReply(TransferId),
    #[error("resume-at for transfer {0}, which was not resumed")]
    UnexpectedResumeAt(TransferId),
    #[error("resume-at for transfer {0} does not list its unfinished files within their sizes")]
    BadResumeAt(TransferId),
    #[error("file-done for file {1} of transfer {0}, which it lacks or finished otherwise")]
    BadFileDone(TransferId, u64),
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, thiserror::Error)]
pub enum StreamRefused {
    #[error("the transfer is not accepted")]
    NotAccepted,
    #[error("no unfinished file with that id")]
    NoSuchFile,
    #[error("the file already has a stream")]
    Busy,
    #[error("the stream starts at {got}, not at {expected}")]
    Offset { expected: u64, got: u64 },
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, thiserror::Error)]
pub enum Overrun {
    #[error("bytes past the announced size")]
    PastSize,
    #[error("the stream ended before the announced size")]
    Short,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum OutgoingPhase {
    Offered,
    Accepted,
    Resuming,
}

#[derive(Debug, Clone)]
struct Tracked {
    id: u64,
    size: u64,
    offset: u64,
    streaming: bool,
    done: Option<bool>,
}

/// The sender's view of one transfer.
#[derive(Debug, Clone)]
pub struct Outgoing {
    id: TransferId,
    files: Vec<Tracked>,
    phase: OutgoingPhase,
}

/// The receiver's view of one transfer.
#[derive(Debug, Clone)]
pub struct Incoming {
    id: TransferId,
    files: Vec<Tracked>,
    accepted: bool,
}

/// Counts one stream's bytes against its file's size.
#[derive(Debug, Clone, Copy)]
pub struct Budget {
    offset: u64,
    size: u64,
}

fn tracked(offer: &Offer) -> Vec<Tracked> {
    offer
        .files
        .iter()
        .map(|file| Tracked { id: file.id, size: file.size, offset: 0, streaming: false, done: None })
        .collect()
}

/// `Some(all ok)` once every file has a result.
fn outcome(files: &[Tracked]) -> Option<bool> {
    files.iter().try_fold(true, |all, file| file.done.map(|ok| all && ok))
}

impl Outgoing {
    pub fn new(offer: &Offer) -> Self {
        Self { id: offer.transfer, files: tracked(offer), phase: OutgoingPhase::Offered }
    }

    pub fn id(&self) -> TransferId {
        self.id
    }

    pub fn phase(&self) -> OutgoingPhase {
        self.phase
    }

    /// The receiver's answer: `Ok(None)` when accepted, with every file starting at 0.
    pub fn on_reply(&mut self, reply: &OfferReply) -> Result<Option<RefuseReason>, TransferError> {
        if self.phase != OutgoingPhase::Offered {
            return Err(TransferError::UnexpectedReply(self.id));
        }
        if !reply.accepted {
            return Ok(Some(reply.reason.unwrap_or(RefuseReason::Declined)));
        }
        self.phase = OutgoingPhase::Accepted;
        Ok(None)
    }

    /// On a new session: whether to send `resume`, which only an accepted transfer does.
    pub fn resume(&mut self) -> bool {
        if self.phase == OutgoingPhase::Offered {
            return false;
        }
        self.phase = OutgoingPhase::Resuming;
        true
    }

    /// Where each unfinished file continues. The receiver must list exactly those, each within its size.
    pub fn on_resume_at(&mut self, at: &ResumeAt) -> Result<Vec<FileOffset>, TransferError> {
        if self.phase != OutgoingPhase::Resuming {
            return Err(TransferError::UnexpectedResumeAt(self.id));
        }
        let unfinished = self.files.iter().filter(|file| file.done.is_none()).count();
        let fits = |offset: &FileOffset| {
            self.files.iter().any(|file| file.id == offset.file && file.done.is_none() && offset.offset <= file.size)
        };
        if at.offsets.len() != unfinished || !at.offsets.iter().all(fits) {
            return Err(TransferError::BadResumeAt(self.id));
        }
        self.phase = OutgoingPhase::Accepted;
        Ok(at.offsets.clone())
    }

    /// Records a file's result; `Ok(Some(all ok))` once every file has one. The same result twice is allowed, since
    /// the receiver repeats them after a resume.
    pub fn on_file_done(&mut self, done: &FileDone) -> Result<Option<bool>, TransferError> {
        let error = TransferError::BadFileDone(self.id, done.file);
        if self.phase == OutgoingPhase::Offered {
            return Err(error);
        }
        let file = self.files.iter_mut().find(|file| file.id == done.file).ok_or_else(|| error.clone())?;
        match file.done {
            Some(ok) if ok != done.ok => return Err(error),
            _ => file.done = Some(done.ok),
        }
        Ok(outcome(&self.files))
    }

    pub fn is_done(&self, file: u64) -> bool {
        self.files.iter().any(|tracked| tracked.id == file && tracked.done.is_some())
    }
}

impl Incoming {
    pub fn new(offer: &Offer) -> Self {
        Self { id: offer.transfer, files: tracked(offer), accepted: false }
    }

    pub fn id(&self) -> TransferId {
        self.id
    }

    pub fn accept(&mut self) {
        self.accepted = true;
    }

    pub fn is_accepted(&self) -> bool {
        self.accepted
    }

    /// Where the next stream for `file` must start: its durable offset.
    pub fn set_offset(&mut self, file: u64, offset: u64) {
        if let Some(tracked) = self.file_mut(file) {
            tracked.offset = offset.min(tracked.size);
        }
    }

    pub fn offset(&self, file: u64) -> Option<u64> {
        self.files.iter().find(|tracked| tracked.id == file).map(|tracked| tracked.offset)
    }

    /// Admits a stream's header, returning the budget its bytes are counted against.
    pub fn on_stream(&mut self, header: &FileData) -> Result<Budget, StreamRefused> {
        if !self.accepted {
            return Err(StreamRefused::NotAccepted);
        }
        let file = self.file_mut(header.file).filter(|file| file.done.is_none()).ok_or(StreamRefused::NoSuchFile)?;
        if file.streaming {
            return Err(StreamRefused::Busy);
        }
        if header.offset != file.offset {
            return Err(StreamRefused::Offset { expected: file.offset, got: header.offset });
        }
        file.streaming = true;
        Ok(Budget { offset: file.offset, size: file.size })
    }

    /// The file's stream is gone; the next one starts at `durable`.
    pub fn stream_ended(&mut self, file: u64, durable: u64) {
        if let Some(tracked) = self.file_mut(file) {
            tracked.streaming = false;
            tracked.offset = durable.min(tracked.size);
        }
    }

    pub fn is_streaming(&self, file: u64) -> bool {
        self.files.iter().any(|tracked| tracked.id == file && tracked.streaming)
    }

    /// Records a file's result; `Some(all ok)` once every file has one.
    pub fn finish(&mut self, file: u64, ok: bool) -> Option<bool> {
        if let Some(tracked) = self.file_mut(file) {
            tracked.streaming = false;
            tracked.done.get_or_insert(ok);
        }
        outcome(&self.files)
    }

    /// The answer to a `resume`: the results so far, then where each unfinished file continues.
    pub fn resume(&self) -> (Vec<FileDone>, ResumeAt) {
        let done = self
            .files
            .iter()
            .filter_map(|file| file.done.map(|ok| FileDone { transfer: self.id, file: file.id, ok }))
            .collect();
        let offsets = self
            .files
            .iter()
            .filter(|file| file.done.is_none())
            .map(|file| FileOffset { file: file.id, offset: file.offset })
            .collect();
        (done, ResumeAt { transfer: self.id, offsets })
    }

    fn file_mut(&mut self, file: u64) -> Option<&mut Tracked> {
        self.files.iter_mut().find(|tracked| tracked.id == file)
    }
}

impl Budget {
    /// Counts `len` more bytes.
    pub fn take(&mut self, len: usize) -> Result<(), Overrun> {
        let len = u64::try_from(len).map_err(|_| Overrun::PastSize)?;
        match self.offset.checked_add(len) {
            Some(next) if next <= self.size => {
                self.offset = next;
                Ok(())
            }
            _ => Err(Overrun::PastSize),
        }
    }

    /// The stream finished: complete, or short of the size.
    pub fn finish(&self) -> Result<(), Overrun> {
        if self.offset == self.size { Ok(()) } else { Err(Overrun::Short) }
    }

    pub fn offset(&self) -> u64 {
        self.offset
    }

    pub fn size(&self) -> u64 {
        self.size
    }
}

#[cfg(test)]
mod tests {
    use ciborium::Value;

    use super::*;
    use crate::message::{DecodeError, Envelope, FileMeta, Message};
    use crate::session::{Inbound, Role, SessionError, SessionState};

    const ID: TransferId = TransferId([7; 16]);

    fn meta(id: u64, size: u64) -> FileMeta {
        FileMeta { id, name: format!("f{id}"), size, mime: "text/plain".to_owned(), sha256: vec![0; 32] }
    }

    fn offer(sizes: &[u64]) -> Offer {
        Offer { transfer: ID, files: (0..).zip(sizes).map(|(id, &size)| meta(id, size)).collect() }
    }

    fn accepted() -> OfferReply {
        OfferReply { transfer: ID, accepted: true, reason: None }
    }

    fn done(file: u64, ok: bool) -> FileDone {
        FileDone { transfer: ID, file, ok }
    }

    fn data(file: u64, offset: u64) -> FileData {
        FileData { transfer: ID, file, offset }
    }

    #[expect(clippy::expect_used, reason = "a cbor value always serializes into memory")]
    fn encode(kind: &str, body: Value) -> Vec<u8> {
        let raw = Value::Map(vec![("type".into(), kind.into()), ("id".into(), 1.into()), ("body".into(), body)]);
        let mut out = Vec::new();
        ciborium::into_writer(&raw, &mut out).expect("cbor into memory");
        out
    }

    fn file_value(id: u64, name: &str, hash_len: usize) -> Value {
        Value::Map(vec![
            ("id".into(), id.into()),
            ("name".into(), name.into()),
            ("size".into(), 3.into()),
            ("mime".into(), "text/plain".into()),
            ("sha256".into(), Value::Bytes(vec![0; hash_len])),
        ])
    }

    fn offer_value(id_len: usize, files: Vec<Value>) -> Value {
        Value::Map(vec![("transfer".into(), Value::Bytes(vec![1; id_len])), ("files".into(), Value::Array(files))])
    }

    #[test]
    fn malformed_offers_and_replies_fail_to_decode() {
        let cases = [
            ("offer", offer_value(16, vec![])),
            ("offer", offer_value(16, vec![file_value(1, "a", 32), file_value(1, "b", 32)])),
            ("offer", offer_value(16, vec![file_value(1, "", 32)])),
            ("offer", offer_value(16, vec![file_value(1, "a", 31)])),
            ("offer", offer_value(15, vec![file_value(1, "a", 32)])),
            (
                "offer-reply",
                Value::Map(vec![
                    ("transfer".into(), Value::Bytes(vec![1; 16])),
                    ("accepted".into(), true.into()),
                    ("reason".into(), "busy".into()),
                ]),
            ),
            (
                "offer-reply",
                Value::Map(vec![("transfer".into(), Value::Bytes(vec![1; 16])), ("accepted".into(), false.into())]),
            ),
            (
                "resume-at",
                Value::Map(vec![
                    ("transfer".into(), Value::Bytes(vec![1; 16])),
                    (
                        "offsets".into(),
                        Value::Array(vec![
                            Value::Map(vec![("file".into(), 1.into()), ("offset".into(), 0.into())]),
                            Value::Map(vec![("file".into(), 1.into()), ("offset".into(), 2.into())]),
                        ]),
                    ),
                ]),
            ),
        ];
        for (kind, body) in cases {
            let decoded = Envelope::from_cbor(&encode(kind, body));
            assert!(matches!(decoded, Err(DecodeError::Invalid(_) | DecodeError::Cbor(_))), "{kind}: {decoded:?}");
        }
        assert!(Envelope::from_cbor(&encode("offer", offer_value(16, vec![file_value(1, "../x", 32)]))).is_ok());
    }

    #[test]
    fn a_reply_needs_an_unanswered_offer() {
        let mut outgoing = Outgoing::new(&offer(&[3]));
        assert_eq!(outgoing.on_reply(&accepted()), Ok(None));
        assert_eq!(outgoing.on_reply(&accepted()), Err(TransferError::UnexpectedReply(ID)));
        let mut declined = Outgoing::new(&offer(&[3]));
        let reply = OfferReply { transfer: ID, accepted: false, reason: Some(RefuseReason::NoSpace) };
        assert_eq!(declined.on_reply(&reply), Ok(Some(RefuseReason::NoSpace)));
    }

    #[test]
    fn a_stream_header_on_the_control_stream_is_unexpected() {
        let envelope = Envelope::new(1, Message::FileData(data(0, 0)));
        assert_eq!(SessionState::new(Role::Desktop).on_message(envelope), Err(SessionError::Unexpected("file-data")));
        let offer = Envelope::new(2, Message::Offer(offer(&[1])));
        assert!(matches!(SessionState::new(Role::Phone).on_message(offer), Ok(Inbound::Transfer(Message::Offer(_)))));
    }

    #[test]
    fn streams_are_refused_unless_they_continue_an_accepted_unfinished_file() {
        let mut incoming = Incoming::new(&offer(&[10, 10]));
        assert!(matches!(incoming.on_stream(&data(0, 0)), Err(StreamRefused::NotAccepted)));
        incoming.accept();
        assert!(matches!(incoming.on_stream(&data(9, 0)), Err(StreamRefused::NoSuchFile)));
        assert!(matches!(incoming.on_stream(&data(0, 4)), Err(StreamRefused::Offset { expected: 0, got: 4 })));
        assert!(incoming.on_stream(&data(0, 0)).is_ok());
        assert!(matches!(incoming.on_stream(&data(0, 0)), Err(StreamRefused::Busy)));
        incoming.stream_ended(0, 6);
        assert!(incoming.on_stream(&data(0, 6)).is_ok());
        assert_eq!(incoming.finish(1, true), None);
        assert!(matches!(incoming.on_stream(&data(1, 0)), Err(StreamRefused::NoSuchFile)));
        assert_eq!(incoming.finish(0, true), Some(true));
    }

    #[test]
    fn bytes_past_the_size_or_a_short_stream_overrun() {
        let mut incoming = Incoming::new(&offer(&[10]));
        incoming.accept();
        let Ok(mut budget) = incoming.on_stream(&data(0, 0)) else { panic!("stream refused") };
        assert_eq!(budget.take(6), Ok(()));
        assert_eq!(budget.finish(), Err(Overrun::Short));
        assert_eq!(budget.take(5), Err(Overrun::PastSize));
        assert_eq!(budget.take(4), Ok(()));
        assert_eq!(budget.finish(), Ok(()));
        assert_eq!(budget.take(1), Err(Overrun::PastSize));
    }

    #[test]
    fn resume_at_must_follow_a_resume_and_list_exactly_the_unfinished_files() {
        let mut outgoing = Outgoing::new(&offer(&[10, 10, 10]));
        let at = |offsets: Vec<(u64, u64)>| ResumeAt {
            transfer: ID,
            offsets: offsets.into_iter().map(|(file, offset)| FileOffset { file, offset }).collect(),
        };
        assert!(!outgoing.resume(), "an unanswered offer is not resumed");
        assert_eq!(outgoing.on_reply(&accepted()), Ok(None));
        assert_eq!(outgoing.on_resume_at(&at(vec![])), Err(TransferError::UnexpectedResumeAt(ID)));
        assert_eq!(outgoing.on_file_done(&done(2, true)), Ok(None));
        assert!(outgoing.resume());
        for bad in [vec![(0, 1)], vec![(0, 1), (1, 11)], vec![(0, 1), (1, 1), (2, 0)], vec![(0, 1), (9, 0)]] {
            assert_eq!(outgoing.on_resume_at(&at(bad)), Err(TransferError::BadResumeAt(ID)));
        }
        assert_eq!(outgoing.on_resume_at(&at(vec![(0, 4), (1, 10)])).map(|offsets| offsets.len()), Ok(2));
        assert_eq!(outgoing.on_resume_at(&at(vec![(0, 4), (1, 10)])), Err(TransferError::UnexpectedResumeAt(ID)));
    }

    #[test]
    fn file_done_must_name_a_file_and_agree_with_itself() {
        let mut outgoing = Outgoing::new(&offer(&[1, 1]));
        assert_eq!(outgoing.on_file_done(&done(0, true)), Err(TransferError::BadFileDone(ID, 0)));
        assert_eq!(outgoing.on_reply(&accepted()), Ok(None));
        assert_eq!(outgoing.on_file_done(&done(5, true)), Err(TransferError::BadFileDone(ID, 5)));
        assert_eq!(outgoing.on_file_done(&done(0, true)), Ok(None));
        assert_eq!(outgoing.on_file_done(&done(0, true)), Ok(None));
        assert_eq!(outgoing.on_file_done(&done(0, false)), Err(TransferError::BadFileDone(ID, 0)));
        assert_eq!(outgoing.on_file_done(&done(1, false)), Ok(Some(false)));
    }

    #[test]
    fn a_receiver_answers_resume_with_results_then_offsets() {
        let mut incoming = Incoming::new(&offer(&[5, 5]));
        incoming.accept();
        incoming.finish(0, false);
        incoming.set_offset(1, 3);
        let (results, at) = incoming.resume();
        assert_eq!(results, vec![done(0, false)]);
        assert_eq!(at.offsets, vec![FileOffset { file: 1, offset: 3 }]);
    }
}
