//! The receiving side: a phone connects, both sides show the PIN, the sender introduces what it shares, the user
//! accepts or declines, and files land in the target directory only once complete.

use std::collections::HashMap;
use std::future::Future;
use std::path::{Path, PathBuf};

use prost::Message as _;
use tokio::fs::File;
use tokio::io::AsyncWriteExt;
use tokio::net::TcpStream;

use crate::connection::{Connection, Incoming, offline, sharing_frame};
use crate::endpoint::{self, Peer};
use crate::error::{Error, Result};
use crate::frame;
use crate::ukey2;
use crate::wire::connections::{self, OfflineFrame, OsInfo, connection_response_frame, os_info, v1_frame};
use crate::wire::sharing::{self, PairedKeyEncryptionFrame, PairedKeyResultFrame, paired_key_result_frame};

/// Longer text is refused rather than buffered.
const MAX_TEXT: i64 = 1024 * 1024;

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum TextKind {
    Text,
    Url,
    Address,
    Phone,
}

#[derive(Debug, Clone)]
pub struct OfferedFile {
    pub name: String,
    pub size: i64,
    pub mime: String,
}

#[derive(Debug, Clone)]
pub struct Offer {
    pub sender: String,
    pub device_type: u8,
    pub pin: String,
    pub files: Vec<OfferedFile>,
    pub texts: Vec<(TextKind, String)>,
}

#[derive(Debug)]
pub enum Outcome {
    Received { files: Vec<PathBuf>, texts: Vec<(TextKind, String)> },
    Declined,
    Cancelled,
}

struct FileSlot {
    size: i64,
    written: i64,
    part: PathBuf,
    target: PathBuf,
    file: Option<File>,
}

/// Runs one inbound connection to its end. `consent` sees the offer (with the PIN) and answers accept or decline.
pub async fn receive<F, Fut>(mut stream: TcpStream, dir: &Path, consent: F) -> Result<Outcome>
where
    F: FnOnce(Offer) -> Fut,
    Fut: Future<Output = bool>,
{
    let peer = connection_request(&mut stream).await?;
    let handshake = ukey2::server(&mut stream).await?;
    exchange_connection_responses(&mut stream).await?;
    let mut conn = Connection::new(stream, &handshake.keys);
    conn.send_sharing(&paired_key_encryption()?).await?;

    let introduction = loop {
        let frame = next_sharing(&mut conn).await?;
        let v1 = frame.v1.unwrap_or_default();
        match v1.r#type.and_then(|t| sharing::v1_frame::FrameType::try_from(t).ok()) {
            Some(sharing::v1_frame::FrameType::PairedKeyEncryption) => conn.send_sharing(&paired_key_result()).await?,
            Some(sharing::v1_frame::FrameType::PairedKeyResult) => {}
            Some(sharing::v1_frame::FrameType::Introduction) => {
                break v1.introduction.ok_or(Error::Protocol("introduction without body"))?;
            }
            Some(sharing::v1_frame::FrameType::Cancel) => return Ok(Outcome::Cancelled),
            other => log::debug!("ignoring sharing frame {other:?} before the introduction"),
        }
    };

    let mut slots = HashMap::new();
    let mut text_ids = HashMap::new();
    let mut offer =
        Offer { sender: peer.name, device_type: peer.device_type, pin: handshake.pin, files: vec![], texts: vec![] };
    for file in &introduction.file_metadata {
        let id = file.payload_id.ok_or(Error::Protocol("file without payload id"))?;
        let size = file.size.filter(|s| *s >= 0).ok_or(Error::Protocol("file without size"))?;
        let name = sanitize(file.name.as_deref().unwrap_or_default());
        offer.files.push(OfferedFile { name: name.clone(), size, mime: file.mime_type.clone().unwrap_or_default() });
        slots.insert(id, (name, size));
    }
    for text in &introduction.text_metadata {
        let id = text.payload_id.ok_or(Error::Protocol("text without payload id"))?;
        if text.size.unwrap_or(0) > MAX_TEXT {
            return Err(Error::Protocol("shared text too large"));
        }
        let kind = match text.r#type.and_then(|t| sharing::text_metadata::Type::try_from(t).ok()) {
            Some(sharing::text_metadata::Type::Url) => TextKind::Url,
            Some(sharing::text_metadata::Type::Address) => TextKind::Address,
            Some(sharing::text_metadata::Type::PhoneNumber) => TextKind::Phone,
            _ => TextKind::Text,
        };
        text_ids.insert(id, kind);
    }
    if slots.is_empty() && text_ids.is_empty() {
        respond(&mut conn, sharing::connection_response_frame::Status::UnsupportedAttachmentType).await?;
        conn.disconnect().await?;
        return Ok(Outcome::Declined);
    }

    // Keep-alives keep flowing while the user decides; a cancel from the sender ends the wait.
    let decision = consent(offer);
    tokio::pin!(decision);
    let accepted = tokio::select! {
        accepted = &mut decision => accepted,
        incoming = conn.recv() => match incoming? {
            Incoming::Disconnected => return Ok(Outcome::Cancelled),
            Incoming::Bytes { data, .. } if is_cancel(&data) => return Ok(Outcome::Cancelled),
            _ => return Err(Error::Protocol("payload before the offer was answered")),
        },
    };
    if !accepted {
        respond(&mut conn, sharing::connection_response_frame::Status::Reject).await?;
        conn.disconnect().await?;
        return Ok(Outcome::Declined);
    }

    let mut files = HashMap::new();
    for (id, (name, size)) in slots {
        let (part, target) = reserve(dir, &name)?;
        let file = File::options().write(true).create_new(true).open(&part).await?;
        files.insert(id, FileSlot { size, written: 0, part, target, file: Some(file) });
    }
    respond(&mut conn, sharing::connection_response_frame::Status::Accept).await?;
    let result = transfer(&mut conn, &mut files, &mut text_ids).await;
    for slot in files.values() {
        if slot.file.is_some() || result.is_err() {
            drop(std::fs::remove_file(&slot.part));
        }
    }
    let (published, texts) = result?;
    conn.disconnect().await?;
    Ok(Outcome::Received { files: published, texts })
}

async fn transfer(
    conn: &mut Connection,
    files: &mut HashMap<i64, FileSlot>,
    texts: &mut HashMap<i64, TextKind>,
) -> Result<(Vec<PathBuf>, Vec<(TextKind, String)>)> {
    let mut published = Vec::new();
    let mut received_texts = Vec::new();
    let mut open = files.len();
    while open > 0 || !texts.is_empty() {
        match conn.recv().await? {
            Incoming::Disconnected => return Err(Error::Protocol("sender left mid-transfer")),
            Incoming::Bytes { id, data } => {
                if let Some(kind) = texts.remove(&id) {
                    received_texts.push((kind, String::from_utf8_lossy(&data).into_owned()));
                } else if is_cancel(&data) {
                    return Err(Error::Protocol("sender cancelled"));
                }
            }
            Incoming::FileChunk { id, offset, body, last, .. } => {
                let slot = files.get_mut(&id).ok_or(Error::Protocol("chunk for an unknown file"))?;
                if offset != slot.written {
                    return Err(Error::Protocol("file chunk out of order"));
                }
                let len = i64::try_from(body.len()).map_err(|_| Error::Protocol("chunk too large"))?;
                if slot.written + len > slot.size {
                    return Err(Error::Protocol("file longer than announced"));
                }
                let file = slot.file.as_mut().ok_or(Error::Protocol("chunk after the last one"))?;
                file.write_all(&body).await?;
                slot.written += len;
                if last {
                    if slot.written != slot.size {
                        return Err(Error::Protocol("file shorter than announced"));
                    }
                    let file = slot.file.take().ok_or(Error::Protocol("chunk after the last one"))?;
                    file.sync_all().await?;
                    published.push(publish(&slot.part, &slot.target)?);
                    open -= 1;
                }
            }
        }
    }
    Ok((published, received_texts))
}

async fn connection_request(stream: &mut TcpStream) -> Result<Peer> {
    let frame = OfflineFrame::decode(frame::read(stream).await?.as_slice())?;
    let v1 = frame.v1.ok_or(Error::Protocol("offline frame without v1"))?;
    if v1.r#type != Some(v1_frame::FrameType::ConnectionRequest as i32) {
        return Err(Error::Protocol("expected a connection request"));
    }
    let request = v1.connection_request.ok_or(Error::Protocol("connection request without body"))?;
    endpoint::parse_info(request.endpoint_info.as_deref().ok_or(Error::Protocol("no endpoint info"))?)
}

/// Both sides send an accepting connection response in the clear; the client's comes first.
async fn exchange_connection_responses(stream: &mut TcpStream) -> Result<()> {
    let frame = OfflineFrame::decode(frame::read(stream).await?.as_slice())?;
    let v1 = frame.v1.ok_or(Error::Protocol("offline frame without v1"))?;
    let accepted = v1.connection_response.and_then(|r| r.response)
        == Some(connection_response_frame::ResponseStatus::Accept as i32);
    if v1.r#type != Some(v1_frame::FrameType::ConnectionResponse as i32) || !accepted {
        return Err(Error::Protocol("the sender did not accept the connection"));
    }
    frame::write(stream, &accepting_response().encode_to_vec()).await
}

// `status` is deprecated in the proto but older Android releases still read it.
#[allow(deprecated)]
pub fn accepting_response() -> OfflineFrame {
    offline(v1_frame::FrameType::ConnectionResponse, |v1| {
        v1.connection_response = Some(connections::ConnectionResponseFrame {
            status: Some(0),
            response: Some(connection_response_frame::ResponseStatus::Accept as i32),
            os_info: Some(OsInfo { r#type: Some(os_info::OsType::Linux as i32) }),
            ..connections::ConnectionResponseFrame::default()
        });
    })
}

async fn next_sharing(conn: &mut Connection) -> Result<sharing::Frame> {
    match conn.recv().await? {
        Incoming::Bytes { data, .. } => Ok(sharing::Frame::decode(data.as_slice())?),
        Incoming::Disconnected => Err(Error::Protocol("sender left during setup")),
        Incoming::FileChunk { .. } => Err(Error::Protocol("file data during setup")),
    }
}

async fn respond(conn: &mut Connection, status: sharing::connection_response_frame::Status) -> Result<()> {
    let frame = sharing_frame(sharing::v1_frame::FrameType::Response, |v1| {
        v1.connection_response =
            Some(sharing::ConnectionResponseFrame { status: Some(status as i32), ..Default::default() });
    });
    conn.send_sharing(&frame).await
}

fn is_cancel(data: &[u8]) -> bool {
    sharing::Frame::decode(data).ok().and_then(|f| f.v1).and_then(|v1| v1.r#type)
        == Some(sharing::v1_frame::FrameType::Cancel as i32)
}

/// Contacts-based pairing is not supported, so the hash and signature are random and the result is always "unable".
pub fn paired_key_encryption() -> Result<sharing::Frame> {
    let (secret_id_hash, signed_data) = (random_bytes::<6>()?, random_bytes::<72>()?);
    Ok(sharing_frame(sharing::v1_frame::FrameType::PairedKeyEncryption, |v1| {
        v1.paired_key_encryption = Some(PairedKeyEncryptionFrame {
            secret_id_hash: Some(secret_id_hash.to_vec()),
            signed_data: Some(signed_data.to_vec()),
            ..PairedKeyEncryptionFrame::default()
        });
    }))
}

pub fn paired_key_result() -> sharing::Frame {
    sharing_frame(sharing::v1_frame::FrameType::PairedKeyResult, |v1| {
        v1.paired_key_result = Some(PairedKeyResultFrame {
            status: Some(paired_key_result_frame::Status::Unable as i32),
            ..PairedKeyResultFrame::default()
        });
    })
}

fn random_bytes<const N: usize>() -> Result<[u8; N]> {
    use ring::rand::SecureRandom as _;
    let mut bytes = [0; N];
    ring::rand::SystemRandom::new().fill(&mut bytes).map_err(|_| Error::Random)?;
    Ok(bytes)
}

/// A bare file name: no directories, no NUL, no leading dot, at most 255 bytes.
pub fn sanitize(name: &str) -> String {
    let base = name.rsplit(['/', '\\']).next().unwrap_or_default();
    let cleaned: String = base.chars().filter(|c| *c != '\0' && !c.is_control()).collect();
    let trimmed = cleaned.trim().trim_start_matches('.');
    let mut end = trimmed.len().min(255);
    while !trimmed.is_char_boundary(end) {
        end -= 1;
    }
    if trimmed[..end].is_empty() { "file".to_owned() } else { trimmed[..end].to_owned() }
}

/// A part file next to a target name no existing file uses; "name (1).ext" style when taken.
fn reserve(dir: &Path, name: &str) -> Result<(PathBuf, PathBuf)> {
    let (stem, ext) = match name.rfind('.') {
        Some(dot) if dot > 0 => (&name[..dot], &name[dot..]),
        _ => (name, ""),
    };
    for n in 0..1000 {
        let candidate = if n == 0 { name.to_owned() } else { format!("{stem} ({n}){ext}") };
        let target = dir.join(&candidate);
        let part = dir.join(format!(".{candidate}.qspart"));
        if !target.exists() && !part.exists() {
            return Ok((part, target));
        }
    }
    Err(Error::Protocol("no free file name"))
}

/// Publishes without ever replacing a file: link to the target, then drop the part name.
fn publish(part: &Path, target: &Path) -> Result<PathBuf> {
    let dir = target.parent().unwrap_or(Path::new("."));
    let name = target.file_name().and_then(|n| n.to_str()).unwrap_or("file");
    let mut target = target.to_path_buf();
    loop {
        match std::fs::hard_link(part, &target) {
            Ok(()) => break,
            Err(e) if e.kind() == std::io::ErrorKind::AlreadyExists => {
                target = reserve(dir, name)?.1;
            }
            Err(e) => return Err(e.into()),
        }
    }
    std::fs::remove_file(part)?;
    Ok(target)
}
