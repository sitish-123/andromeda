use serde_json::{Map, Value, json};
use tokio::io::{AsyncBufRead, AsyncBufReadExt};

use crate::idl::SCHEMA_VERSION;

/// Longest accepted line on either socket, newline excluded.
pub const MAX_LINE: usize = 1 << 20;

pub enum Line {
    Complete,
    TooLong,
    Eof,
}

/// Reads one line into `buf` without the newline. A longer line is skipped up to its newline, so memory stays bounded.
pub async fn read_line<R: AsyncBufRead + Unpin>(reader: &mut R, buf: &mut Vec<u8>) -> std::io::Result<Line> {
    buf.clear();
    let mut too_long = false;
    loop {
        let chunk = reader.fill_buf().await?;
        if chunk.is_empty() {
            return Ok(Line::Eof);
        }
        let (take, done) = chunk.iter().position(|&b| b == b'\n').map_or((chunk.len(), false), |i| (i, true));
        if !too_long {
            if buf.len() + take > MAX_LINE {
                too_long = true;
                buf.clear();
            } else {
                buf.extend_from_slice(&chunk[..take]);
            }
        }
        reader.consume(take + usize::from(done));
        if done {
            return Ok(if too_long { Line::TooLong } else { Line::Complete });
        }
    }
}

#[derive(Clone, Copy)]
pub enum Code {
    InvalidArgument,
    NotFound,
    Unavailable,
}

impl Code {
    fn as_str(self) -> &'static str {
        match self {
            Self::InvalidArgument => "invalid_argument",
            Self::NotFound => "not_found",
            Self::Unavailable => "unavailable",
        }
    }
}

pub fn ok(value: Value) -> Value {
    reply(true, "value", value)
}

fn reply(ok: bool, key: &str, body: Value) -> Value {
    let mut fields = Map::new();
    fields.insert("ok".into(), ok.into());
    fields.insert(key.into(), body);
    fields.insert("schema_version".into(), SCHEMA_VERSION.into());
    Value::Object(fields)
}

pub fn error_object(code: Code, message: &str, hint: &str, example: &str) -> Value {
    json!({ "code": code.as_str(), "message": message, "hint": hint, "example": example })
}

pub fn err(code: Code, message: &str, hint: &str, example: &str) -> Value {
    failed(error_object(code, message, hint, example))
}

pub fn failed(error: Value) -> Value {
    reply(false, "error", error)
}

/// The three candidates nearest to `word` by edit distance.
pub fn closest<'a>(word: &str, candidates: impl IntoIterator<Item = &'a str>) -> Vec<&'a str> {
    let mut ranked: Vec<(usize, &str)> = candidates.into_iter().map(|c| (distance(word, c), c)).collect();
    ranked.sort_unstable();
    ranked.dedup();
    ranked.into_iter().take(3).map(|(_, c)| c).collect()
}

fn distance(a: &str, b: &str) -> usize {
    let b: Vec<char> = b.chars().collect();
    let mut row: Vec<usize> = (0..=b.len()).collect();
    for (i, ca) in a.chars().enumerate() {
        let mut diagonal = row[0];
        row[0] = i + 1;
        for (j, cb) in b.iter().enumerate() {
            let next = (diagonal + usize::from(ca != *cb)).min(row[j] + 1).min(row[j + 1] + 1);
            diagonal = row[j + 1];
            row[j + 1] = next;
        }
    }
    row[b.len()]
}
