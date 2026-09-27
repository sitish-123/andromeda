use ciborium::Value;
use serde::{Deserialize, Serialize};

use crate::pairing::{MAC_LEN, SPAKE_MSG_LEN};

pub const MAX_NAME_LEN: usize = 64;

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

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct Empty {}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Message {
    Hello(Hello),
    PairSpake(PairSpake),
    PairConfirm(PairConfirm),
    Unpair,
}

impl Message {
    pub fn kind(&self) -> &'static str {
        match self {
            Self::Hello(_) => "hello",
            Self::PairSpake(_) => "pair-spake",
            Self::PairConfirm(_) => "pair-confirm",
            Self::Unpair => "unpair",
        }
    }

    fn body(&self) -> Result<Value, ciborium::value::Error> {
        match self {
            Self::Hello(body) => Value::serialized(body),
            Self::PairSpake(body) => Value::serialized(body),
            Self::PairConfirm(body) => Value::serialized(body),
            Self::Unpair => Value::serialized(&Empty {}),
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
            Self::Unpair => true,
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
