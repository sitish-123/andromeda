//! The desktop side of a phone's contacts and texts: kept per phone, asked for page by page when it connects and
//! whenever it says they changed, and texts sent through it. A phone that refuses (its messages switch is off, or
//! Android denies it) has none here.

use std::collections::HashMap;
use std::time::Duration;

use link_core::identity::DeviceId;
use link_core::proto::message::{
    self, Contact, ContactsList, Message, MessagesRefusal, Text, TextSend, TextSent, TextsList,
};
use tokio::sync::oneshot;
use tokio::time::Instant;

/// A phone slower than this for one page drops the pull.
const PAGE_TIMEOUT: Duration = Duration::from_secs(15);
/// The phone waits up to 60 s for Android to report a text sent.
const SEND_TIMEOUT: Duration = Duration::from_secs(75);
/// Contacts one pull gathers before it is dropped, so a hostile phone cannot grow it without bound.
const MAX_CONTACTS: usize = 16 * 1024;
/// The newest texts kept per phone.
const MAX_TEXTS: usize = 256;

/// Contacts as `(name, numbers)`.
pub type Contacts = Vec<(String, Vec<String>)>;
/// Texts as `(id, thread, address, body, date in Unix ms, sent)`, newest first.
pub type Texts = Vec<(u64, u64, String, String, u64, bool)>;

pub type SendReply = oneshot::Sender<Result<(), SendFailure>>;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SendFailure {
    Refused(MessagesRefusal),
    NotConnected,
    TimedOut,
}

enum Gathered {
    Contacts(Vec<Contact>),
    Texts(Vec<Text>),
}

struct Pull {
    device: DeviceId,
    gathered: Gathered,
    deadline: Instant,
}

struct Sending {
    device: DeviceId,
    reply: SendReply,
    deadline: Instant,
}

/// What one page changed.
#[derive(Default)]
pub struct Changed {
    /// The request for the next page.
    pub next: Option<Message>,
    /// Whether the D-Bus snapshot changed.
    pub publish: bool,
    /// Texts the phone received since the last pull, newer than every text kept.
    pub received: Vec<Text>,
}

#[derive(Default)]
pub struct Messages {
    next: u64,
    pulls: HashMap<u64, Pull>,
    sends: HashMap<u64, Sending>,
    contacts: HashMap<DeviceId, Vec<Contact>>,
    texts: HashMap<DeviceId, Vec<Text>>,
}

impl Messages {
    /// The request that starts pulling `device`'s contacts.
    pub fn pull_contacts(&mut self, device: DeviceId) -> Message {
        let req = self.start(device, Gathered::Contacts(Vec::new()));
        Message::ContactsList(ContactsList { req, cursor: None })
    }

    /// The request that starts pulling `device`'s newest texts.
    pub fn pull_texts(&mut self, device: DeviceId) -> Message {
        let req = self.start(device, Gathered::Texts(Vec::new()));
        Message::TextsList(TextsList { req, cursor: None })
    }

    /// Registers a pull, replacing one of the same kind from `device`.
    fn start(&mut self, device: DeviceId, gathered: Gathered) -> u64 {
        self.next += 1;
        let kind = std::mem::discriminant(&gathered);
        self.pulls.retain(|_, pull| pull.device != device || std::mem::discriminant(&pull.gathered) != kind);
        self.pulls.insert(self.next, Pull { device, gathered, deadline: Instant::now() + PAGE_TIMEOUT });
        self.next
    }

    pub fn on_contacts(&mut self, from: &DeviceId, page: message::Contacts) -> Changed {
        let Some(pull) = self.pulls.get_mut(&page.req).filter(|pull| pull.device == *from) else {
            return Changed::default();
        };
        let Gathered::Contacts(gathered) = &mut pull.gathered else { return Changed::default() };
        if let Some(reason) = page.refused {
            log::info!("{from}: contacts refused: {}", reason.as_str());
            self.pulls.remove(&page.req);
            return Changed { publish: self.contacts.remove(from).is_some(), ..Changed::default() };
        }
        gathered.extend(page.contacts);
        pull.deadline = Instant::now() + PAGE_TIMEOUT;
        if gathered.len() > MAX_CONTACTS {
            log::info!("{from}: more than {MAX_CONTACTS} contacts; dropped");
            self.pulls.remove(&page.req);
            return Changed::default();
        }
        if let Some(cursor) = page.next {
            let next = Message::ContactsList(ContactsList { req: page.req, cursor: Some(cursor) });
            return Changed { next: Some(next), ..Changed::default() };
        }
        let Some(Pull { gathered: Gathered::Contacts(gathered), .. }) = self.pulls.remove(&page.req) else {
            return Changed::default();
        };
        let publish = self.contacts.get(from) != Some(&gathered);
        self.contacts.insert(from.clone(), gathered);
        Changed { publish, ..Changed::default() }
    }

    pub fn on_texts(&mut self, from: &DeviceId, page: message::Texts) -> Changed {
        let Some(pull) = self.pulls.get_mut(&page.req).filter(|pull| pull.device == *from) else {
            return Changed::default();
        };
        let Gathered::Texts(gathered) = &mut pull.gathered else { return Changed::default() };
        if let Some(reason) = page.refused {
            log::info!("{from}: texts refused: {}", reason.as_str());
            self.pulls.remove(&page.req);
            return Changed { publish: self.texts.remove(from).is_some(), ..Changed::default() };
        }
        gathered.extend(page.texts);
        gathered.truncate(MAX_TEXTS);
        pull.deadline = Instant::now() + PAGE_TIMEOUT;
        if let Some(cursor) = page.next.filter(|_| gathered.len() < MAX_TEXTS) {
            let next = Message::TextsList(TextsList { req: page.req, cursor: Some(cursor) });
            return Changed { next: Some(next), ..Changed::default() };
        }
        let Some(Pull { gathered: Gathered::Texts(gathered), .. }) = self.pulls.remove(&page.req) else {
            return Changed::default();
        };
        let newest = self.texts.get(from).map(|kept| kept.iter().map(|text| text.date).max().unwrap_or(0));
        let received = match newest {
            Some(newest) => gathered.iter().filter(|text| !text.sent && text.date > newest).cloned().collect(),
            None => Vec::new(),
        };
        let publish = self.texts.get(from) != Some(&gathered);
        self.texts.insert(from.clone(), gathered);
        Changed { publish, received, ..Changed::default() }
    }

    /// Registers a text to send through `device` and returns the message that asks it.
    pub fn send_text(&mut self, device: DeviceId, to: String, body: String, reply: SendReply) -> (u64, Message) {
        self.next += 1;
        self.sends.insert(self.next, Sending { device, reply, deadline: Instant::now() + SEND_TIMEOUT });
        (self.next, Message::TextSend(TextSend { req: self.next, to, body }))
    }

    pub fn on_sent(&mut self, from: &DeviceId, sent: &TextSent) {
        if self.sends.get(&sent.req).is_some_and(|sending| sending.device == *from) {
            self.answer_send(sent.req, sent.refused.map(SendFailure::Refused));
        }
    }

    /// Answers the send `req`: sent without a failure.
    pub fn answer_send(&mut self, req: u64, failure: Option<SendFailure>) {
        if let Some(sending) = self.sends.remove(&req) {
            let _ = sending.reply.send(failure.map_or(Ok(()), Err));
        }
    }

    /// Device id to its contacts, the D-Bus `Contacts` property.
    pub fn contacts(&self) -> Vec<(String, Contacts)> {
        let mut all: Vec<_> = self
            .contacts
            .iter()
            .map(|(id, contacts)| {
                let list = contacts.iter().map(|contact| (contact.name.clone(), contact.numbers.clone())).collect();
                (id.to_string(), list)
            })
            .collect();
        all.sort();
        all
    }

    /// Device id to its newest texts, the D-Bus `Texts` property.
    pub fn texts(&self) -> Vec<(String, Texts)> {
        let mut all: Vec<_> = self
            .texts
            .iter()
            .map(|(id, texts)| {
                let list = texts
                    .iter()
                    .map(|text| (text.id, text.thread, text.address.clone(), text.body.clone(), text.date, text.sent))
                    .collect();
                (id.to_string(), list)
            })
            .collect();
        all.sort();
        all
    }

    /// Fails what waits on `device`'s session; its contacts and texts stay.
    pub fn disconnected(&mut self, device: &DeviceId) {
        let waiting: Vec<u64> =
            self.sends.iter().filter(|(_, send)| send.device == *device).map(|(req, _)| *req).collect();
        for req in waiting {
            self.answer_send(req, Some(SendFailure::NotConnected));
        }
        self.pulls.retain(|_, pull| pull.device != *device);
    }

    pub fn forget(&mut self, device: &DeviceId) {
        self.disconnected(device);
        self.contacts.remove(device);
        self.texts.remove(device);
    }

    pub fn deadline(&self) -> Option<Instant> {
        let pulls = self.pulls.values().map(|pull| pull.deadline);
        pulls.chain(self.sends.values().map(|send| send.deadline)).min()
    }

    pub fn expire(&mut self) {
        let now = Instant::now();
        self.pulls.retain(|_, pull| {
            let alive = pull.deadline > now;
            if !alive {
                log::info!("{}: a contacts or texts pull timed out", pull.device);
            }
            alive
        });
        let late: Vec<u64> = self.sends.iter().filter(|(_, send)| send.deadline <= now).map(|(req, _)| *req).collect();
        for req in late {
            self.answer_send(req, Some(SendFailure::TimedOut));
        }
    }
}
