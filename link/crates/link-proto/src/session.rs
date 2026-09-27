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
    /// A file transfer or clipboard message, for the transfer actor to judge.
    Transfer(Message),
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
            message @ (Message::Offer(_)
            | Message::OfferReply(_)
            | Message::Resume(_)
            | Message::ResumeAt(_)
            | Message::Cancel(_)
            | Message::FileDone(_)
            | Message::ClipOffer(_)
            | Message::ClipPull(_)) => Ok(Inbound::Transfer(message)),
            other => Err(SessionError::Unexpected(other.kind())),
        }
    }
}

#[cfg(test)]
mod tests {
    use ciborium::Value;

    use super::*;
    use crate::message::{
        DecodeError, Hello, MAX_SHARE_LEN, PairConfirm, PairSpake, ShareAck, ShareKind, ShareRejected,
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

    #[test]
    fn unpair_is_legal_only_towards_the_desktop() {
        assert_eq!(SessionState::new(Role::Desktop).on_message(envelope(1, Message::Unpair)), Ok(Inbound::Unpair));
        assert_eq!(
            SessionState::new(Role::Phone).on_message(envelope(1, Message::Unpair)),
            Err(SessionError::Unexpected("unpair"))
        );
    }
}
