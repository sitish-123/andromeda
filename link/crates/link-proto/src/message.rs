use std::fmt::{self, Write as _};

use ciborium::Value;
use serde::{Deserialize, Serialize};

use crate::pairing::{MAC_LEN, SPAKE_MSG_LEN};

pub const MAX_NAME_LEN: usize = 64;
/// Leaves room for the envelope, so a share always fits one control frame.
pub const MAX_SHARE_LEN: usize = 60 * 1024;

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Hello {
    pub version: u32,
    pub name: String,
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub addresses: Vec<String>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum PairMethod {
    Code,
    Qr,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct PairSpake {
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub method: Option<PairMethod>,
    #[serde(with = "serde_bytes")]
    pub msg: Vec<u8>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct PairConfirm {
    #[serde(with = "serde_bytes")]
    pub mac: Vec<u8>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum ShareKind {
    Text,
    Link,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Share {
    pub kind: ShareKind,
    pub text: String,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct ShareAck {
    /// The envelope id of the share this acknowledges.
    pub of: u64,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, thiserror::Error)]
pub enum ShareRejected {
    #[error("the text is empty")]
    Empty,
    #[error("the text is longer than {MAX_SHARE_LEN} bytes")]
    TooLong,
    #[error("a link must be an http or https url")]
    NotWebLink,
}

impl ShareKind {
    pub fn as_str(self) -> &'static str {
        match self {
            Self::Text => "text",
            Self::Link => "link",
        }
    }

    pub fn parse(text: &str) -> Option<Self> {
        match text {
            "text" => Some(Self::Text),
            "link" => Some(Self::Link),
            _ => None,
        }
    }
}

impl Share {
    /// The rules of a share, checked by the sender before sending and by the receiver while decoding.
    pub fn check(&self) -> Result<(), ShareRejected> {
        if self.text.is_empty() {
            return Err(ShareRejected::Empty);
        }
        if self.text.len() > MAX_SHARE_LEN {
            return Err(ShareRejected::TooLong);
        }
        if self.kind == ShareKind::Link && !is_web_link(&self.text) {
            return Err(ShareRejected::NotWebLink);
        }
        Ok(())
    }
}

/// An absolute http or https URL: scheme in any case, something after it, and no whitespace or control characters.
fn is_web_link(text: &str) -> bool {
    let rest = ["https://", "http://"].iter().find_map(|scheme| {
        let head = text.get(..scheme.len())?;
        head.eq_ignore_ascii_case(scheme).then(|| &text[scheme.len()..])
    });
    rest.is_some_and(|rest| !rest.is_empty()) && !text.chars().any(|c| c.is_whitespace() || c.is_control())
}

/// A transfer's id: 16 random bytes chosen by the sender.
#[derive(Clone, Copy, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct TransferId(pub [u8; TRANSFER_ID_LEN]);

pub const TRANSFER_ID_LEN: usize = 16;
pub const SHA256_LEN: usize = 32;
/// Longer names are cut to 255 bytes on receipt; this only bounds what a frame may carry.
pub const MAX_WIRE_NAME_LEN: usize = 4096;
pub const MAX_MIME_LEN: usize = 255;

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct FileMeta {
    pub id: u64,
    pub name: String,
    pub size: u64,
    pub mime: String,
    #[serde(with = "serde_bytes")]
    pub sha256: Vec<u8>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Offer {
    pub transfer: TransferId,
    pub files: Vec<FileMeta>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "kebab-case")]
pub enum RefuseReason {
    Declined,
    NoSpace,
    TooLarge,
    Busy,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct OfferReply {
    pub transfer: TransferId,
    pub accepted: bool,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub reason: Option<RefuseReason>,
}

/// The body of `resume` and `cancel`.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct TransferRef {
    pub transfer: TransferId,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct FileOffset {
    pub file: u64,
    pub offset: u64,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct ResumeAt {
    pub transfer: TransferId,
    pub offsets: Vec<FileOffset>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct FileDone {
    pub transfer: TransferId,
    pub file: u64,
    pub ok: bool,
}

/// The first frame of a file's unidirectional stream; the bytes from `offset` to the file's size follow.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct FileData {
    pub transfer: TransferId,
    pub file: u64,
    pub offset: u64,
}

pub const MAX_CLIP_MIMES: usize = 16;
pub const MAX_CLIP_SIZE: u64 = 64 << 20;

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct ClipOffer {
    pub id: u64,
    pub mimes: Vec<String>,
    pub size: u64,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub text: Option<String>,
}

/// The body of `clip-pull`, and of `clip-data`, the first frame of the stream that answers it.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct ClipPull {
    pub id: u64,
    pub mime: String,
}

impl ClipOffer {
    pub fn is_valid(&self) -> bool {
        let mimes_ok = (1..=MAX_CLIP_MIMES).contains(&self.mimes.len())
            && self.mimes.iter().all(|mime| (1..=MAX_MIME_LEN).contains(&mime.len()));
        let text_ok = self.text.as_ref().is_none_or(|text| {
            (1..=MAX_SHARE_LEN).contains(&text.len()) && self.mimes.iter().any(|mime| is_text_mime(mime))
        });
        mimes_ok && text_ok && self.size <= MAX_CLIP_SIZE
    }
}

/// `text/plain`, with or without parameters.
pub fn is_text_mime(mime: &str) -> bool {
    mime.split(';').next().is_some_and(|base| base.trim().eq_ignore_ascii_case("text/plain"))
}

impl RefuseReason {
    pub fn as_str(self) -> &'static str {
        match self {
            Self::Declined => "declined",
            Self::NoSpace => "no-space",
            Self::TooLarge => "too-large",
            Self::Busy => "busy",
        }
    }
}

impl TransferId {
    pub fn to_hex(self) -> String {
        self.0.iter().fold(String::with_capacity(2 * TRANSFER_ID_LEN), |mut hex, byte| {
            let _ = write!(hex, "{byte:02x}");
            hex
        })
    }

    pub fn parse_hex(text: &str) -> Option<Self> {
        if text.len() != 2 * TRANSFER_ID_LEN || !text.is_ascii() {
            return None;
        }
        let mut id = [0; TRANSFER_ID_LEN];
        for (index, byte) in id.iter_mut().enumerate() {
            *byte = u8::from_str_radix(&text[2 * index..2 * index + 2], 16).ok()?;
        }
        Some(Self(id))
    }
}

impl fmt::Debug for TransferId {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.to_hex())
    }
}

impl fmt::Display for TransferId {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.to_hex())
    }
}

impl Serialize for TransferId {
    fn serialize<S: serde::Serializer>(&self, serializer: S) -> Result<S::Ok, S::Error> {
        serializer.serialize_bytes(&self.0)
    }
}

impl<'de> Deserialize<'de> for TransferId {
    fn deserialize<D: serde::Deserializer<'de>>(deserializer: D) -> Result<Self, D::Error> {
        let bytes = serde_bytes::ByteBuf::deserialize(deserializer)?;
        let id = <[u8; TRANSFER_ID_LEN]>::try_from(bytes.as_slice())
            .map_err(|_| serde::de::Error::invalid_length(bytes.len(), &"16 bytes"))?;
        Ok(Self(id))
    }
}

impl Offer {
    /// The rules of an offer, checked by the sender before sending and by the receiver while decoding.
    pub fn is_valid(&self) -> bool {
        !self.files.is_empty()
            && unique(self.files.iter().map(|file| file.id))
            && self.files.iter().all(FileMeta::is_valid)
    }

    pub fn total_size(&self) -> u64 {
        self.files.iter().map(|file| file.size).fold(0, u64::saturating_add)
    }
}

impl FileMeta {
    fn is_valid(&self) -> bool {
        (1..=MAX_WIRE_NAME_LEN).contains(&self.name.len())
            && (1..=MAX_MIME_LEN).contains(&self.mime.len())
            && self.sha256.len() == SHA256_LEN
    }
}

fn unique(ids: impl Iterator<Item = u64>) -> bool {
    let mut ids: Vec<u64> = ids.collect();
    let count = ids.len();
    ids.sort_unstable();
    ids.dedup();
    ids.len() == count
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct Empty {}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Message {
    Hello(Hello),
    PairSpake(PairSpake),
    PairConfirm(PairConfirm),
    Unpair,
    Share(Share),
    ShareAck(ShareAck),
    Offer(Offer),
    OfferReply(OfferReply),
    Resume(TransferRef),
    ResumeAt(ResumeAt),
    Cancel(TransferRef),
    FileDone(FileDone),
    FileData(FileData),
    ClipOffer(ClipOffer),
    ClipPull(ClipPull),
    ClipData(ClipPull),
}

impl Message {
    pub fn kind(&self) -> &'static str {
        match self {
            Self::Hello(_) => "hello",
            Self::PairSpake(_) => "pair-spake",
            Self::PairConfirm(_) => "pair-confirm",
            Self::Unpair => "unpair",
            Self::Share(_) => "share",
            Self::ShareAck(_) => "share-ack",
            Self::Offer(_) => "offer",
            Self::OfferReply(_) => "offer-reply",
            Self::Resume(_) => "resume",
            Self::ResumeAt(_) => "resume-at",
            Self::Cancel(_) => "cancel",
            Self::FileDone(_) => "file-done",
            Self::FileData(_) => "file-data",
            Self::ClipOffer(_) => "clip-offer",
            Self::ClipPull(_) => "clip-pull",
            Self::ClipData(_) => "clip-data",
        }
    }

    fn body(&self) -> Result<Value, ciborium::value::Error> {
        match self {
            Self::Hello(body) => Value::serialized(body),
            Self::PairSpake(body) => Value::serialized(body),
            Self::PairConfirm(body) => Value::serialized(body),
            Self::Unpair => Value::serialized(&Empty {}),
            Self::Share(body) => Value::serialized(body),
            Self::ShareAck(body) => Value::serialized(body),
            Self::Offer(body) => Value::serialized(body),
            Self::OfferReply(body) => Value::serialized(body),
            Self::Resume(body) | Self::Cancel(body) => Value::serialized(body),
            Self::ResumeAt(body) => Value::serialized(body),
            Self::FileDone(body) => Value::serialized(body),
            Self::FileData(body) => Value::serialized(body),
            Self::ClipOffer(body) => Value::serialized(body),
            Self::ClipPull(body) | Self::ClipData(body) => Value::serialized(body),
        }
    }

    fn from_parts(kind: &str, body: &Value) -> Result<Self, DecodeError> {
        let message = match kind {
            "hello" => Self::Hello(body.deserialized()?),
            "pair-spake" => Self::PairSpake(body.deserialized()?),
            "pair-confirm" => Self::PairConfirm(body.deserialized()?),
            "unpair" => {
                body.deserialized::<Empty>()?;
                Self::Unpair
            }
            "share" => Self::Share(body.deserialized()?),
            "share-ack" => Self::ShareAck(body.deserialized()?),
            "offer" => Self::Offer(body.deserialized()?),
            "offer-reply" => Self::OfferReply(body.deserialized()?),
            "resume" => Self::Resume(body.deserialized()?),
            "resume-at" => Self::ResumeAt(body.deserialized()?),
            "cancel" => Self::Cancel(body.deserialized()?),
            "file-done" => Self::FileDone(body.deserialized()?),
            "file-data" => Self::FileData(body.deserialized()?),
            "clip-offer" => Self::ClipOffer(body.deserialized()?),
            "clip-pull" => Self::ClipPull(body.deserialized()?),
            "clip-data" => Self::ClipData(body.deserialized()?),
            other => return Err(DecodeError::UnknownType(other.to_owned())),
        };
        message.validate()?;
        Ok(message)
    }

    fn validate(&self) -> Result<(), DecodeError> {
        let valid = match self {
            Self::Hello(hello) => (1..=MAX_NAME_LEN).contains(&hello.name.chars().count()),
            Self::PairSpake(spake) => spake.msg.len() == SPAKE_MSG_LEN,
            Self::PairConfirm(confirm) => confirm.mac.len() == MAC_LEN,
            Self::Unpair
            | Self::ShareAck(_)
            | Self::Resume(_)
            | Self::Cancel(_)
            | Self::FileDone(_)
            | Self::FileData(_) => true,
            Self::ClipPull(pull) | Self::ClipData(pull) => (1..=MAX_MIME_LEN).contains(&pull.mime.len()),
            Self::ClipOffer(offer) => offer.is_valid(),
            Self::Share(share) => share.check().is_ok(),
            Self::Offer(offer) => offer.is_valid(),
            Self::OfferReply(reply) => reply.accepted == reply.reason.is_none(),
            Self::ResumeAt(at) => unique(at.offsets.iter().map(|offset| offset.file)),
        };
        if valid { Ok(()) } else { Err(DecodeError::Invalid(self.kind())) }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Envelope {
    pub id: u64,
    pub message: Message,
}

#[derive(Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct RawEnvelope {
    #[serde(rename = "type")]
    kind: String,
    id: u64,
    body: Value,
}

#[derive(Debug, thiserror::Error)]
pub enum DecodeError {
    #[error("malformed cbor: {0}")]
    Cbor(String),
    #[error("unknown message type {0:?}")]
    UnknownType(String),
    #[error("{0} message violates the schema")]
    Invalid(&'static str),
}

impl From<ciborium::value::Error> for DecodeError {
    fn from(error: ciborium::value::Error) -> Self {
        Self::Cbor(error.to_string())
    }
}

impl Envelope {
    pub fn new(id: u64, message: Message) -> Self {
        Self { id, message }
    }

    #[expect(clippy::expect_used, reason = "our own message types always serialize into memory")]
    pub fn to_cbor(&self) -> Vec<u8> {
        let raw = RawEnvelope {
            kind: self.message.kind().to_owned(),
            id: self.id,
            body: self.message.body().expect("message body serializes"),
        };
        let mut out = Vec::new();
        ciborium::into_writer(&raw, &mut out).expect("envelope serializes");
        out
    }

    pub fn from_cbor(bytes: &[u8]) -> Result<Self, DecodeError> {
        let mut reader = std::io::Cursor::new(bytes);
        let raw: RawEnvelope =
            ciborium::from_reader(&mut reader).map_err(|error| DecodeError::Cbor(error.to_string()))?;
        if usize::try_from(reader.position()).ok() != Some(bytes.len()) {
            return Err(DecodeError::Cbor("trailing bytes after the envelope".to_owned()));
        }
        Ok(Self { id: raw.id, message: Message::from_parts(&raw.kind, &raw.body)? })
    }
}
