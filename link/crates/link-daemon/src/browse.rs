//! The desktop side of browsing a phone: requests to it, paired with its answers by `req`. A listing follows the
//! phone's cursors to the end; a read gathers its `fs-data` frames.

use std::collections::HashMap;
use std::time::Duration;

use link_core::identity::DeviceId;
use link_core::proto::message::{FsEntry, FsList, FsRead, FsRefusal, FsStat, FsThumb, MAX_FS_READ, Message};
use tokio::sync::oneshot;
use tokio::time::Instant;

/// A phone slower than this for one answer, or one page, fails the request.
const ANSWER_TIMEOUT: Duration = Duration::from_secs(15);
/// Entries one listing gathers before it fails, so a hostile phone cannot grow it without bound.
const MAX_ENTRIES: usize = 16 * 1024;

pub enum Request {
    List {
        path: String,
    },
    Stat {
        path: String,
    },
    Read {
        path: String,
        offset: u64,
        len: u32,
    },
    Thumb {
        path: String,
        px: u16,
    },
    /// One entry of a listing, standing in for `Stat` with a phone from before it.
    Find {
        parent: String,
        name: String,
    },
}

impl Request {
    /// What a phone from before `fs-stat` understands: a stat lists the parent, a thumbnail has no stand-in.
    pub fn for_older_phone(self) -> Result<Self, Failure> {
        match self {
            Self::Stat { path } => {
                let (parent, name) = path.rsplit_once('/').ok_or(Failure::Refused(FsRefusal::NotFound))?;
                let parent = if parent.is_empty() { "/" } else { parent };
                Ok(Self::Find { parent: parent.to_owned(), name: name.to_owned() })
            }
            Self::Thumb { .. } => Err(Failure::Refused(FsRefusal::NotFound)),
            other => Ok(other),
        }
    }
}

#[derive(Debug)]
pub enum Answer {
    Entries(Vec<FsEntry>),
    Data(Vec<u8>),
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Failure {
    Refused(FsRefusal),
    NotConnected,
    TimedOut,
    /// The phone answered against the protocol: a listing too long, a read longer than asked.
    Malformed,
}

pub type Reply = oneshot::Sender<Result<Answer, Failure>>;

enum Step {
    Next(FsList),
    Done,
    Fail(Failure),
    Wait,
}

struct Pending {
    device: DeviceId,
    path: String,
    /// The one entry a `Find` keeps.
    pick: Option<String>,
    gathered: Gathered,
    reply: Reply,
    deadline: Instant,
}

enum Gathered {
    Entries(Vec<FsEntry>),
    Data { data: Vec<u8>, want: usize },
}

#[derive(Default)]
pub struct Browsing {
    next: u64,
    pending: HashMap<u64, Pending>,
}

impl Browsing {
    /// Registers `request` and returns the message that asks the phone.
    pub fn start(&mut self, device: DeviceId, request: Request, reply: Reply) -> (u64, Message) {
        self.next += 1;
        let req = self.next;
        let mut pick = None;
        let (path, gathered, message) = match request {
            Request::List { path } => {
                let message = Message::FsList(FsList { req, path: path.clone(), cursor: None });
                (path, Gathered::Entries(Vec::new()), message)
            }
            Request::Stat { path } => {
                let message = Message::FsStat(FsStat { req, path: path.clone() });
                (path, Gathered::Entries(Vec::new()), message)
            }
            Request::Read { path, offset, len } => {
                let message = Message::FsRead(FsRead { req, path: path.clone(), offset, len });
                (path, Gathered::Data { data: Vec::new(), want: len as usize }, message)
            }
            Request::Find { parent, name } => {
                pick = Some(name);
                let message = Message::FsList(FsList { req, path: parent.clone(), cursor: None });
                (parent, Gathered::Entries(Vec::new()), message)
            }
            Request::Thumb { path, px } => {
                let message = Message::FsThumb(FsThumb { req, path: path.clone(), px });
                (path, Gathered::Data { data: Vec::new(), want: MAX_FS_READ as usize }, message)
            }
        };
        let deadline = Instant::now() + ANSWER_TIMEOUT;
        self.pending.insert(req, Pending { device, path, pick, gathered, reply, deadline });
        (req, message)
    }

    pub fn fail(&mut self, req: u64, failure: Failure) {
        if let Some(pending) = self.pending.remove(&req) {
            drop(pending.reply.send(Err(failure)));
        }
    }

    /// Takes one answer from `from`; returns the request for the next page of a listing, if there is one.
    pub fn on_answer(&mut self, from: &DeviceId, answer: Message) -> Option<Message> {
        let req = match &answer {
            Message::FsEntries(page) => page.req,
            Message::FsData(data) => data.req,
            Message::FsError(error) => error.req,
            _ => return None,
        };
        let pending = self.pending.get_mut(&req).filter(|pending| pending.device == *from)?;
        pending.deadline = Instant::now() + ANSWER_TIMEOUT;
        let step = match (answer, &mut pending.gathered) {
            (Message::FsError(error), _) => Step::Fail(Failure::Refused(error.reason)),
            (Message::FsEntries(page), Gathered::Entries(entries)) => {
                entries.extend(page.entries);
                match page.next {
                    _ if entries.len() > MAX_ENTRIES => Step::Fail(Failure::Malformed),
                    Some(cursor) => Step::Next(FsList { req, path: pending.path.clone(), cursor: Some(cursor) }),
                    None => Step::Done,
                }
            }
            (Message::FsData(chunk), Gathered::Data { data, want }) => {
                data.extend_from_slice(&chunk.data);
                if data.len() > *want {
                    Step::Fail(Failure::Malformed)
                } else if chunk.last {
                    Step::Done
                } else {
                    Step::Wait
                }
            }
            _ => Step::Fail(Failure::Malformed),
        };
        match step {
            Step::Next(list) => return Some(Message::FsList(list)),
            Step::Done => self.finish(req),
            Step::Fail(failure) => self.fail(req, failure),
            Step::Wait => {}
        }
        None
    }

    fn finish(&mut self, req: u64) {
        let Some(pending) = self.pending.remove(&req) else { return };
        let answer = match (pending.gathered, pending.pick) {
            (Gathered::Entries(entries), Some(name)) => match entries.into_iter().find(|entry| entry.name == name) {
                Some(entry) => Ok(Answer::Entries(vec![entry])),
                None => Err(Failure::Refused(FsRefusal::NotFound)),
            },
            (Gathered::Entries(entries), None) => Ok(Answer::Entries(entries)),
            (Gathered::Data { data, .. }, _) => Ok(Answer::Data(data)),
        };
        drop(pending.reply.send(answer));
    }

    pub fn deadline(&self) -> Option<Instant> {
        self.pending.values().map(|pending| pending.deadline).min()
    }

    pub fn expire(&mut self) {
        let now = Instant::now();
        let late: Vec<u64> =
            self.pending.iter().filter(|(_, pending)| pending.deadline <= now).map(|(req, _)| *req).collect();
        for req in late {
            self.fail(req, Failure::TimedOut);
        }
    }

    pub fn disconnected(&mut self, device: &DeviceId) {
        let gone: Vec<u64> =
            self.pending.iter().filter(|(_, pending)| pending.device == *device).map(|(req, _)| *req).collect();
        for req in gone {
            self.fail(req, Failure::NotConnected);
        }
    }
}
