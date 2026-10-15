//! The phone side of contacts and texts: the app's, a page at a time, to desktops whose messages switch is on, and
//! texts those desktops send through the phone.

use std::sync::Arc;

use link_proto::message::{
    Contact, Contacts, ContactsList, MAX_CALLER_LEN, MAX_CONTACT_NUMBERS, MAX_CONTACTS_PAGE, MAX_NUMBER_LEN,
    MAX_TEXT_BODY, MAX_TEXTS_PAGE, Message, MessagesRefusal, Text, TextSend, TextSent, Texts, TextsList,
};

use crate::session::SessionHandle;

/// Called off the actor; each may block. `Denied` while Android does not let the app read or send them.
pub trait Messages: Send + Sync {
    /// Up to `limit` contacts from `offset`, in a stable order.
    fn contacts(&self, offset: u32, limit: u32) -> Result<Vec<Contact>, MessagesRefusal>;
    /// Up to `limit` texts from `offset`, newest first.
    fn texts(&self, offset: u32, limit: u32) -> Result<Vec<Text>, MessagesRefusal>;
    /// Returns once Android reports the text sent.
    fn send_text(&self, to: &str, body: &str) -> Result<(), MessagesRefusal>;
}

pub enum Request {
    Contacts(ContactsList),
    Texts(TextsList),
    Send(TextSend),
}

impl Request {
    pub fn of(message: Message) -> Option<Self> {
        match message {
            Message::ContactsList(list) => Some(Self::Contacts(list)),
            Message::TextsList(list) => Some(Self::Texts(list)),
            Message::TextSend(send) => Some(Self::Send(send)),
            _ => None,
        }
    }

    pub fn refused(&self, reason: MessagesRefusal) -> Message {
        let refused = Some(reason);
        match self {
            Self::Contacts(list) => {
                Message::Contacts(Contacts { req: list.req, contacts: Vec::new(), next: None, refused })
            }
            Self::Texts(list) => Message::Texts(Texts { req: list.req, texts: Vec::new(), next: None, refused }),
            Self::Send(send) => Message::TextSent(TextSent { req: send.req, refused }),
        }
    }
}

/// Answers one request off the actor.
pub async fn answer(messages: Arc<dyn Messages>, session: SessionHandle, request: Request) {
    let reply = tokio::task::spawn_blocking(move || match &request {
        Request::Contacts(list) => contacts_page(&*messages, list).unwrap_or_else(|reason| request.refused(reason)),
        Request::Texts(list) => texts_page(&*messages, list).unwrap_or_else(|reason| request.refused(reason)),
        Request::Send(send) => match messages.send_text(&send.to, &send.body) {
            Ok(()) => Message::TextSent(TextSent { req: send.req, refused: None }),
            Err(reason) => request.refused(reason),
        },
    })
    .await;
    let Ok(reply) = reply else { return log::warn!("a contacts or texts request panicked") };
    if let Err(error) = session.send(reply).await {
        log::info!("answering a contacts or texts request: {error}");
    }
}

fn contacts_page(messages: &dyn Messages, list: &ContactsList) -> Result<Message, MessagesRefusal> {
    let (offset, limit) = (list.cursor.unwrap_or(0), page_len(MAX_CONTACTS_PAGE));
    let contacts = messages.contacts(offset, limit)?;
    let next = (contacts.len() >= MAX_CONTACTS_PAGE).then(|| offset + limit);
    let contacts = contacts.iter().take(MAX_CONTACTS_PAGE).filter_map(fitted).collect();
    Ok(Message::Contacts(Contacts { req: list.req, contacts, next, refused: None }))
}

fn texts_page(messages: &dyn Messages, list: &TextsList) -> Result<Message, MessagesRefusal> {
    let (offset, limit) = (list.cursor.unwrap_or(0), page_len(MAX_TEXTS_PAGE));
    let texts = messages.texts(offset, limit)?;
    let next = (texts.len() >= MAX_TEXTS_PAGE).then(|| offset + limit);
    let texts = texts
        .into_iter()
        .take(MAX_TEXTS_PAGE)
        .map(|text| Text {
            address: cut(text.address.trim(), MAX_NUMBER_LEN),
            body: cut(&text.body, MAX_TEXT_BODY),
            ..text
        })
        .filter(|text| !text.address.is_empty())
        .collect();
    Ok(Message::Texts(Texts { req: list.req, texts, next, refused: None }))
}

fn page_len(len: usize) -> u32 {
    len.try_into().unwrap_or(u32::MAX)
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
