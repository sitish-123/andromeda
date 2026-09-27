//! A running session (`link/ARCHITECTURE.md`, Session messages): which messages are legal once both hellos are
//! exchanged, and which shares still wait for their ack.

use std::collections::HashSet;

use crate::message::{Envelope, Message, Share};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Role {
    Phone,
    Desktop,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Inbound {
    /// Deliver it, then acknowledge `id`.
    Share {
        id: u64,
        share: Share,
    },
    Acked {
        of: u64,
    },
    /// The phone unpaired; only a desktop receives this.
    Unpair,
    /// An unacknowledged feature message, already checked to travel in this direction.
    Deliver(Message),
}

#[derive(Debug, Clone, PartialEq, Eq, thiserror::Error)]
pub enum SessionError {
    #[error("unexpected {0} message in a session")]
    Unexpected(&'static str),
    #[error("acknowledgement of {0}, which is not an outstanding share")]
    UnknownAck(u64),
}

pub struct SessionState {
    role: Role,
    outstanding: HashSet<u64>,
}

impl SessionState {
    pub fn new(role: Role) -> Self {
        Self { role, outstanding: HashSet::new() }
    }

    /// Records a share sent in envelope `id`; it stays outstanding until acknowledged, even past the sender's timeout,
    /// so a late ack is still legal.
    pub fn sent_share(&mut self, id: u64) {
        self.outstanding.insert(id);
    }

    pub fn on_message(&mut self, envelope: Envelope) -> Result<Inbound, SessionError> {
        match envelope.message {
            Message::Share(share) => Ok(Inbound::Share { id: envelope.id, share }),
            Message::ShareAck(ack) if self.outstanding.remove(&ack.of) => Ok(Inbound::Acked { of: ack.of }),
            Message::ShareAck(ack) => Err(SessionError::UnknownAck(ack.of)),
            Message::Unpair if self.role == Role::Desktop => Ok(Inbound::Unpair),
            message if receives(self.role, &message) => Ok(Inbound::Deliver(message)),
            other => Err(SessionError::Unexpected(other.kind())),
        }
    }
}

/// Which feature messages each role may receive.
fn receives(role: Role, message: &Message) -> bool {
    match message {
        Message::NotificationPosted(_) | Message::NotificationRemoved(_) => role == Role::Desktop,
        Message::NotificationAction(_) | Message::NotificationDismiss(_) => role == Role::Phone,
        _ => false,
    }
}

#[cfg(test)]
mod tests {
    use ciborium::Value;

    use super::*;
    use crate::message::{
        DecodeError, Hello, MAX_ACTION_LEN, MAX_ACTIONS, MAX_ICON_LEN, MAX_NOTIFICATION_ID_LEN, MAX_SHARE_LEN,
        MAX_TEXT_LEN, MAX_TITLE_LEN, NotificationAction, NotificationButton, NotificationDismiss, NotificationPosted,
        NotificationRemoved, PairConfirm, PairSpake, ShareAck, ShareKind, ShareRejected,
    };

    fn envelope(id: u64, message: Message) -> Envelope {
        Envelope::new(id, message)
    }

    fn share(kind: ShareKind, text: &str) -> Share {
        Share { kind, text: text.to_owned() }
    }

    /// A share envelope encoded without the sender's checks, as a hostile peer would send it.
    #[expect(clippy::expect_used, reason = "a cbor value always serializes into memory")]
    fn raw_share(kind: &str, text: &str) -> Vec<u8> {
        let body = Value::Map(vec![("kind".into(), kind.into()), ("text".into(), text.into())]);
        let raw = Value::Map(vec![("type".into(), "share".into()), ("id".into(), 7.into()), ("body".into(), body)]);
        let mut out = Vec::new();
        ciborium::into_writer(&raw, &mut out).expect("cbor into memory");
        out
    }

    #[test]
    fn handshake_messages_inside_a_session_are_unexpected() {
        let messages = [
            Message::Hello(Hello { version: 1, name: "phone".to_owned(), addresses: Vec::new() }),
            Message::PairSpake(PairSpake { method: None, msg: vec![0; 33] }),
            Message::PairConfirm(PairConfirm { mac: vec![0; 32] }),
        ];
        for role in [Role::Phone, Role::Desktop] {
            for message in messages.clone() {
                let kind = message.kind();
                let result = SessionState::new(role).on_message(envelope(0, message));
                assert_eq!(result, Err(SessionError::Unexpected(kind)));
            }
        }
    }

    #[test]
    fn an_ack_without_an_outstanding_share_is_rejected() {
        let mut session = SessionState::new(Role::Phone);
        let ack = |of| envelope(9, Message::ShareAck(ShareAck { of }));
        assert_eq!(session.on_message(ack(3)), Err(SessionError::UnknownAck(3)));
        session.sent_share(3);
        assert_eq!(session.on_message(ack(3)), Ok(Inbound::Acked { of: 3 }));
        assert_eq!(session.on_message(ack(3)), Err(SessionError::UnknownAck(3)));
    }

    #[test]
    fn a_share_is_delivered_with_its_envelope_id() {
        let text = share(ShareKind::Link, "HTTPS://example.org/a?b=c");
        let result = SessionState::new(Role::Desktop).on_message(envelope(4, Message::Share(text.clone())));
        assert_eq!(result, Ok(Inbound::Share { id: 4, share: text }));
    }

    #[test]
    fn a_share_breaking_the_rules_fails_to_decode() {
        let long = "a".repeat(MAX_SHARE_LEN + 1);
        let cases = [
            ("text", ""),
            ("text", long.as_str()),
            ("file", "notes"),
            ("link", "ftp://example.org/"),
            ("link", "javascript:alert(1)"),
            ("link", "https://"),
            ("link", "https://example.org/a b"),
            ("link", "https://example.org/\n"),
        ];
        for (kind, text) in cases {
            let decoded = Envelope::from_cbor(&raw_share(kind, text));
            assert!(
                matches!(decoded, Err(DecodeError::Invalid("share") | DecodeError::Cbor(_))),
                "{kind} {text:?} decoded as {decoded:?}"
            );
        }
        assert!(Envelope::from_cbor(&raw_share("text", &"a".repeat(MAX_SHARE_LEN))).is_ok());
        assert!(Envelope::from_cbor(&raw_share("text", "ftp://example.org/")).is_ok());
    }

    #[test]
    fn the_sender_check_names_the_rule() {
        assert_eq!(share(ShareKind::Text, "").check(), Err(ShareRejected::Empty));
        assert_eq!(share(ShareKind::Text, &"a".repeat(MAX_SHARE_LEN + 1)).check(), Err(ShareRejected::TooLong));
        assert_eq!(share(ShareKind::Link, "file:///etc/passwd").check(), Err(ShareRejected::NotWebLink));
        assert_eq!(share(ShareKind::Link, "http://10.0.0.1:8080/").check(), Ok(()));
    }

    fn posted(id: &str) -> NotificationPosted {
        NotificationPosted {
            id: id.to_owned(),
            app: "Messages".to_owned(),
            title: "Ann".to_owned(),
            text: "See you at 6".to_owned(),
            icon: Some(vec![0x89; 64]),
            actions: vec![NotificationButton { id: "0".to_owned(), label: "Reply".to_owned(), reply: true }],
        }
    }

    #[test]
    fn notification_messages_travel_one_way() {
        let from_phone = [
            Message::NotificationPosted(posted("k")),
            Message::NotificationRemoved(NotificationRemoved { id: "k".to_owned() }),
        ];
        let from_desktop = [
            Message::NotificationAction(NotificationAction {
                id: "k".to_owned(),
                action: "0".to_owned(),
                reply_text: Some("ok".to_owned()),
            }),
            Message::NotificationDismiss(NotificationDismiss { id: "k".to_owned() }),
        ];
        for (receiver, legal, illegal) in
            [(Role::Desktop, &from_phone, &from_desktop), (Role::Phone, &from_desktop, &from_phone)]
        {
            for message in legal.iter().cloned() {
                let result = SessionState::new(receiver).on_message(envelope(2, message.clone()));
                assert_eq!(result, Ok(Inbound::Deliver(message)));
            }
            for message in illegal.iter().cloned() {
                let kind = message.kind();
                let result = SessionState::new(receiver).on_message(envelope(2, message));
                assert_eq!(result, Err(SessionError::Unexpected(kind)));
            }
        }
    }

    #[test]
    fn a_notification_breaking_the_limits_fails_to_decode() {
        let with = |change: fn(&mut NotificationPosted)| {
            let mut post = posted("k");
            change(&mut post);
            Message::NotificationPosted(post)
        };
        let action = |reply: &str| {
            Message::NotificationAction(NotificationAction {
                id: "k".to_owned(),
                action: "0".to_owned(),
                reply_text: Some(reply.to_owned()),
            })
        };
        let cases = [
            with(|post| post.id.clear()),
            with(|post| post.id = "k".repeat(MAX_NOTIFICATION_ID_LEN + 1)),
            with(|post| post.app.clear()),
            with(|post| post.title = "t".repeat(MAX_TITLE_LEN + 1)),
            with(|post| post.text = "t".repeat(MAX_TEXT_LEN + 1)),
            with(|post| post.icon = Some(Vec::new())),
            with(|post| post.icon = Some(vec![0; MAX_ICON_LEN + 1])),
            with(|post| post.actions = vec![post.actions[0].clone(); MAX_ACTIONS + 1]),
            with(|post| post.actions[0].label.clear()),
            with(|post| post.actions[0].id = "a".repeat(MAX_ACTION_LEN + 1)),
            action(""),
            action(&"r".repeat(MAX_TEXT_LEN + 1)),
            Message::NotificationDismiss(NotificationDismiss { id: String::new() }),
        ];
        for message in cases {
            let kind = message.kind();
            let decoded = Envelope::from_cbor(&Envelope::new(1, message).to_cbor());
            assert!(matches!(decoded, Err(DecodeError::Invalid(k)) if k == kind), "{kind} decoded as {decoded:?}");
        }
        let full = with(|post| {
            post.icon = Some(vec![0; MAX_ICON_LEN]);
            post.text = "t".repeat(MAX_TEXT_LEN);
            post.actions = vec![post.actions[0].clone(); MAX_ACTIONS];
        });
        assert!(Envelope::from_cbor(&Envelope::new(1, full).to_cbor()).is_ok());
    }

    #[test]
    fn unpair_is_legal_only_towards_the_desktop() {
        assert_eq!(SessionState::new(Role::Desktop).on_message(envelope(1, Message::Unpair)), Ok(Inbound::Unpair));
        assert_eq!(
            SessionState::new(Role::Phone).on_message(envelope(1, Message::Unpair)),
            Err(SessionError::Unexpected("unpair"))
        );
    }
}
