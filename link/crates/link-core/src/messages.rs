//! The phone side of contacts: the app's address book, a page at a time, to desktops whose messages switch is on.

use std::sync::Arc;

use link_proto::message::{
    Contact, Contacts, MAX_CALLER_LEN, MAX_CONTACT_NUMBERS, MAX_CONTACTS_PAGE, MAX_NUMBER_LEN, Message, MessagesRefusal,
};

use crate::session::SessionHandle;

pub trait Messages: Send + Sync {
    /// Up to `limit` contacts from `offset`, in a stable order; `Denied` while Android does not let the app read them.
    fn contacts(&self, offset: u32, limit: u32) -> Result<Vec<Contact>, MessagesRefusal>;
}

pub fn refused(req: u64, reason: MessagesRefusal) -> Message {
    Message::Contacts(Contacts { req, contacts: Vec::new(), next: None, refused: Some(reason) })
}

/// Answers one `contacts-list` off the actor.
pub async fn answer(messages: Arc<dyn Messages>, session: SessionHandle, req: u64, cursor: Option<u32>) {
    let offset = cursor.unwrap_or(0);
    let limit = MAX_CONTACTS_PAGE.try_into().unwrap_or(u32::MAX);
    let page = tokio::task::spawn_blocking(move || messages.contacts(offset, limit))
        .await
        .unwrap_or(Err(MessagesRefusal::Failed));
    let reply = match page {
        Ok(contacts) => {
            let next = (contacts.len() >= MAX_CONTACTS_PAGE).then(|| offset + limit);
            let contacts = contacts.iter().take(MAX_CONTACTS_PAGE).filter_map(fitted).collect();
            Message::Contacts(Contacts { req, contacts, next, refused: None })
        }
        Err(reason) => refused(req, reason),
    };
    if let Err(error) = session.send(reply).await {
        log::info!("answering a contacts request: {error}");
    }
}

/// `contact` cut to the protocol's limits; None without a name or a number.
fn fitted(contact: &Contact) -> Option<Contact> {
    let name = cut(contact.name.trim(), MAX_CALLER_LEN);
    let numbers: Vec<String> = contact
        .numbers
        .iter()
        .map(|number| cut(number.trim(), MAX_NUMBER_LEN))
        .filter(|number| !number.is_empty())
        .take(MAX_CONTACT_NUMBERS)
        .collect();
    (!name.is_empty() && !numbers.is_empty()).then_some(Contact { name, numbers })
}

/// At most `max` bytes of `text`, cut on a character boundary.
fn cut(text: &str, max: usize) -> String {
    let end = (0..=max.min(text.len())).rev().find(|&end| text.is_char_boundary(end)).unwrap_or(0);
    text[..end].to_owned()
}
