//! The desktop side of a phone's contacts: kept per phone, asked for page by page when it connects and whenever it says
//! they changed. A phone that refuses (its messages switch is off, or Android denies it) has none here.

use std::collections::HashMap;
use std::time::Duration;

use link_core::identity::DeviceId;
use link_core::proto::message::{self, Contact, ContactsList, Message};
use tokio::time::Instant;

/// A phone slower than this for one page drops the pull.
const PAGE_TIMEOUT: Duration = Duration::from_secs(15);
/// Contacts one pull gathers before it is dropped, so a hostile phone cannot grow it without bound.
const MAX_CONTACTS: usize = 16 * 1024;

/// Contacts as `(name, numbers)`.
pub type Contacts = Vec<(String, Vec<String>)>;

struct Pull {
    device: DeviceId,
    gathered: Vec<Contact>,
    deadline: Instant,
}

#[derive(Default)]
pub struct Messages {
    next: u64,
    pulls: HashMap<u64, Pull>,
    contacts: HashMap<DeviceId, Vec<Contact>>,
}

impl Messages {
    /// The request that starts pulling `device`'s contacts.
    pub fn pull_contacts(&mut self, device: DeviceId) -> Message {
        self.next += 1;
        self.pulls.retain(|_, pull| pull.device != device);
        let deadline = Instant::now() + PAGE_TIMEOUT;
        self.pulls.insert(self.next, Pull { device, gathered: Vec::new(), deadline });
        Message::ContactsList(ContactsList { req: self.next, cursor: None })
    }

    /// Takes one page from `from`: the request for the next page, if any, and whether the kept contacts changed.
    pub fn on_contacts(&mut self, from: &DeviceId, page: message::Contacts) -> (Option<Message>, bool) {
        let Some(pull) = self.pulls.get_mut(&page.req).filter(|pull| pull.device == *from) else {
            return (None, false);
        };
        if let Some(reason) = page.refused {
            log::info!("{from}: contacts refused: {}", reason.as_str());
            self.pulls.remove(&page.req);
            return (None, self.contacts.remove(from).is_some());
        }
        pull.gathered.extend(page.contacts);
        pull.deadline = Instant::now() + PAGE_TIMEOUT;
        if pull.gathered.len() > MAX_CONTACTS {
            log::info!("{from}: more than {MAX_CONTACTS} contacts; dropped");
            self.pulls.remove(&page.req);
            return (None, false);
        }
        if let Some(cursor) = page.next {
            return (Some(Message::ContactsList(ContactsList { req: page.req, cursor: Some(cursor) })), false);
        }
        let Some(pull) = self.pulls.remove(&page.req) else { return (None, false) };
        let changed = self.contacts.get(from) != Some(&pull.gathered);
        self.contacts.insert(from.clone(), pull.gathered);
        (None, changed)
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

    pub fn forget(&mut self, device: &DeviceId) -> bool {
        self.pulls.retain(|_, pull| pull.device != *device);
        self.contacts.remove(device).is_some()
    }

    pub fn deadline(&self) -> Option<Instant> {
        self.pulls.values().map(|pull| pull.deadline).min()
    }

    pub fn expire(&mut self) {
        let now = Instant::now();
        self.pulls.retain(|_, pull| {
            let alive = pull.deadline > now;
            if !alive {
                log::info!("{}: the contacts pull timed out", pull.device);
            }
            alive
        });
    }
}
