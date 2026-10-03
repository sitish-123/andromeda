//! A running session (`link/ARCHITECTURE.md`, Session messages): which messages are legal once both hellos are
//! exchanged, and which shares still wait for their ack.

use std::collections::HashSet;

use crate::message::{Envelope, Message, Share, Status};

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
    /// The phone's battery and network; only a desktop receives this.
    Status(Status),
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
            Message::Status(status) if self.role == Role::Desktop => Ok(Inbound::Status(status)),
            message @ (Message::Offer(_)
            | Message::OfferReply(_)
            | Message::Resume(_)
            | Message::ResumeAt(_)
            | Message::Cancel(_)
            | Message::FileDone(_)
            | Message::ClipOffer(_)
            | Message::ClipPull(_)) => Ok(Inbound::Transfer(message)),
            message if receives(self.role, &message) => Ok(Inbound::Deliver(message)),
            other => Err(SessionError::Unexpected(other.kind())),
        }
    }
}

/// Which feature messages each role may receive.
fn receives(role: Role, message: &Message) -> bool {
    match message {
        Message::NotificationPosted(_)
        | Message::NotificationRemoved(_)
        | Message::Call(_)
        | Message::Hotspot(_)
        | Message::Punch(_)
        | Message::WifiDirect(_)
        | Message::BtPairing(_)
        | Message::MirrorStarted(_)
        | Message::FsEntries(_)
        | Message::FsData(_)
        | Message::FsError(_) => role == Role::Desktop,
        Message::NotificationAction(_)
        | Message::NotificationDismiss(_)
        | Message::CallAction(_)
        | Message::HotspotRequest
        | Message::HotspotJoined(_)
        | Message::WifiDirectReady(_)
        | Message::MirrorRequest
        | Message::MirrorInput(_)
        | Message::MirrorKeyframe
        | Message::FsList(_)
        | Message::FsRead(_) => role == Role::Phone,
        Message::HotspotEnd(_)
        | Message::MirrorStop(_)
        | Message::MediaPlayer(_)
        | Message::MediaGone(_)
        | Message::MediaCommand(_)
        | Message::Ring(_)
        | Message::Ringing(_) => true,
        _ => false,
    }
}

#[cfg(test)]
mod tests {
    use ciborium::Value;

    use super::*;
    use crate::message::{
        Call, CallAction, CallActionKind, CallState, DecodeError, Hello, MAX_ACTION_LEN, MAX_ACTIONS, MAX_ARTWORK_LEN,
        MAX_CALLER_LEN, MAX_ICON_LEN, MAX_METADATA_LEN, MAX_NOTIFICATION_ID_LEN, MAX_NUMBER_LEN, MAX_PLAYER_LEN,
        MAX_SHARE_LEN, MAX_TEXT_LEN, MAX_TITLE_LEN, MAX_VOLUME, MediaCommand, MediaCommandKind, MediaGone, MediaPlayer,
        NotificationAction, NotificationButton, NotificationDismiss, NotificationPosted, NotificationRemoved,
        PairConfirm, PairSpake, PlaybackState, Ring, Ringing, ShareAck, ShareKind, ShareRejected,
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

    fn player() -> MediaPlayer {
        MediaPlayer {
            player: "spotify".to_owned(),
            name: "Spotify".to_owned(),
            state: PlaybackState::Playing,
            title: "Song".to_owned(),
            artist: "Artist".to_owned(),
            album: String::new(),
            length_ms: Some(180_000),
            position_ms: 1_000,
            volume: Some(50),
            artwork: None,
            can: vec![MediaCommandKind::PlayPause, MediaCommandKind::Seek],
        }
    }

    #[test]
    fn media_and_ring_messages_travel_both_ways() {
        let command = MediaCommand { player: "p".to_owned(), command: MediaCommandKind::Next, value: None };
        let messages = [
            Message::MediaPlayer(player()),
            Message::MediaGone(MediaGone { player: "p".to_owned() }),
            Message::MediaCommand(command),
            Message::Ring(Ring { on: true }),
            Message::Ringing(Ringing { on: false }),
        ];
        for role in [Role::Phone, Role::Desktop] {
            for message in messages.clone() {
                let result = SessionState::new(role).on_message(envelope(3, message.clone()));
                assert_eq!(result, Ok(Inbound::Deliver(message)));
            }
        }
    }

    #[test]
    #[expect(clippy::expect_used, reason = "a cbor value always serializes into memory")]
    fn media_breaking_the_rules_fails_to_decode() {
        let with = |change: fn(&mut MediaPlayer)| {
            let mut media = player();
            change(&mut media);
            Message::MediaPlayer(media)
        };
        let command = |command, value| Message::MediaCommand(MediaCommand { player: "p".to_owned(), command, value });
        let cases = [
            with(|media| media.player.clear()),
            with(|media| media.name = "n".repeat(MAX_PLAYER_LEN + 1)),
            with(|media| media.title = "t".repeat(MAX_METADATA_LEN + 1)),
            with(|media| media.volume = Some(MAX_VOLUME + 1)),
            with(|media| media.artwork = Some(vec![0; MAX_ARTWORK_LEN + 1])),
            with(|media| media.artwork = Some(Vec::new())),
            with(|media| media.can = vec![MediaCommandKind::Next, MediaCommandKind::Next]),
            command(MediaCommandKind::Seek, None),
            command(MediaCommandKind::Volume, Some(101)),
            command(MediaCommandKind::Play, Some(1)),
            Message::MediaGone(MediaGone { player: String::new() }),
        ];
        for message in cases {
            let kind = message.kind();
            let decoded = Envelope::from_cbor(&Envelope::new(1, message).to_cbor());
            assert!(matches!(decoded, Err(DecodeError::Invalid(k)) if k == kind), "{kind} decoded as {decoded:?}");
        }
        let body = Value::Map(vec![("player".into(), "p".into()), ("command".into(), "rewind".into())]);
        let raw =
            Value::Map(vec![("type".into(), "media-command".into()), ("id".into(), 1.into()), ("body".into(), body)]);
        let mut bytes = Vec::new();
        ciborium::into_writer(&raw, &mut bytes).expect("cbor into memory");
        assert!(matches!(Envelope::from_cbor(&bytes), Err(DecodeError::Cbor(_))));
        assert!(Envelope::from_cbor(&Envelope::new(1, command(MediaCommandKind::Volume, Some(100))).to_cbor()).is_ok());
    }

    #[test]
    fn call_messages_travel_one_way_within_their_limits() {
        let call = |number: Option<&str>, name: Option<&str>| {
            Message::Call(Call {
                state: CallState::Ringing,
                number: number.map(str::to_owned),
                name: name.map(str::to_owned),
            })
        };
        let action = Message::CallAction(CallAction { action: CallActionKind::Decline });
        let ringing = call(Some("+15551234"), Some("Ada"));
        assert_eq!(
            SessionState::new(Role::Desktop).on_message(envelope(1, ringing.clone())),
            Ok(Inbound::Deliver(ringing.clone()))
        );
        assert_eq!(
            SessionState::new(Role::Phone).on_message(envelope(1, ringing)),
            Err(SessionError::Unexpected("call"))
        );
        assert_eq!(
            SessionState::new(Role::Phone).on_message(envelope(1, action.clone())),
            Ok(Inbound::Deliver(action.clone()))
        );
        assert_eq!(
            SessionState::new(Role::Desktop).on_message(envelope(1, action)),
            Err(SessionError::Unexpected("call-action"))
        );
        let long_number = "5".repeat(MAX_NUMBER_LEN + 1);
        let long_name = "n".repeat(MAX_CALLER_LEN + 1);
        for message in [call(Some(""), None), call(Some(&long_number), None), call(None, Some(&long_name))] {
            assert!(matches!(
                Envelope::from_cbor(&Envelope::new(1, message).to_cbor()),
                Err(DecodeError::Invalid("call"))
            ));
        }
        assert!(Envelope::from_cbor(&Envelope::new(1, call(None, None)).to_cbor()).is_ok());
    }

    #[test]
    fn status_is_legal_only_towards_the_desktop_and_within_range() {
        use crate::message::{NetworkKind, Status};
        let status = Status { battery: 42, charging: true, network: NetworkKind::Cellular };
        let desktop = SessionState::new(Role::Desktop).on_message(envelope(1, Message::Status(status)));
        assert_eq!(desktop, Ok(Inbound::Status(status)));
        let phone = SessionState::new(Role::Phone).on_message(envelope(1, Message::Status(status)));
        assert_eq!(phone, Err(SessionError::Unexpected("status")));
        let body = Value::Map(vec![
            ("battery".into(), 101.into()),
            ("charging".into(), false.into()),
            ("network".into(), "wifi".into()),
        ]);
        let raw = Value::Map(vec![("type".into(), "status".into()), ("id".into(), 1.into()), ("body".into(), body)]);
        let mut bytes = Vec::new();
        assert!(ciborium::into_writer(&raw, &mut bytes).is_ok());
        assert!(matches!(Envelope::from_cbor(&bytes), Err(DecodeError::Invalid("status"))));
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
