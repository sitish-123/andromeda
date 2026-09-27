//! The sending side: connect to a receiver, show the PIN, introduce the files, and stream them once accepted.

use std::net::SocketAddr;
use std::path::PathBuf;

use prost::Message as _;
use tokio::io::AsyncReadExt;
use tokio::net::TcpStream;

use crate::connection::{CHUNK, Connection, Incoming, offline, random_id, sharing_frame};
use crate::endpoint::Endpoint;
use crate::error::{Error, Result};
use crate::frame;
use crate::receive::{accepting_response, paired_key_encryption, paired_key_result};
use crate::ukey2;
use crate::wire::connections::payload_transfer_frame::payload_header::PayloadType;
use crate::wire::connections::{ConnectionRequestFrame, OfflineFrame, connection_response_frame, v1_frame};
use crate::wire::sharing::{self, FileMetadata, IntroductionFrame};

#[derive(Debug, PartialEq, Eq)]
pub enum SendOutcome {
    Sent,
    Rejected(i32),
}

/// Misbehaviour for protocol tests: offer every file under `name`, or stream `extra_bytes` past the announced size.
#[derive(Debug, Default, Clone)]
pub struct Hostile {
    pub name: Option<String>,
    pub extra_bytes: usize,
}

/// `on_pin` sees the 4-digit code as soon as the key exchange ends, before anything is offered.
pub async fn send(
    addr: SocketAddr,
    own: &Endpoint,
    paths: &[PathBuf],
    hostile: &Hostile,
    on_pin: impl FnOnce(&str),
) -> Result<SendOutcome> {
    let mut stream = TcpStream::connect(addr).await?;
    let info = own.info()?;
    let request = offline(v1_frame::FrameType::ConnectionRequest, |v1| {
        v1.connection_request = Some(ConnectionRequestFrame {
            endpoint_id: Some(own.id_str()),
            endpoint_name: Some(own.name.clone()),
            endpoint_info: Some(info),
            ..ConnectionRequestFrame::default()
        });
    });
    frame::write(&mut stream, &request.encode_to_vec()).await?;
    let handshake = ukey2::client(&mut stream).await?;
    on_pin(&handshake.pin);
    frame::write(&mut stream, &accepting_response().encode_to_vec()).await?;
    let response = OfflineFrame::decode(frame::read(&mut stream).await?.as_slice())?;
    let accepted = response.v1.and_then(|v1| v1.connection_response).and_then(|r| r.response)
        == Some(connection_response_frame::ResponseStatus::Accept as i32);
    if !accepted {
        return Err(Error::Protocol("the receiver refused the connection"));
    }

    let mut conn = Connection::new(stream, &handshake.keys);
    conn.send_sharing(&paired_key_encryption()?).await?;
    let mut files = Vec::new();
    for path in paths {
        let size =
            i64::try_from(tokio::fs::metadata(path).await?.len()).map_err(|_| Error::Protocol("file too large"))?;
        let name = hostile
            .name
            .clone()
            .unwrap_or_else(|| path.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default());
        files.push((random_id()?, path, name, size));
    }
    loop {
        let data = match conn.recv().await? {
            Incoming::Bytes { data, .. } => data,
            Incoming::Disconnected => return Err(Error::Protocol("the receiver left")),
            Incoming::FileChunk { .. } => return Err(Error::Protocol("the receiver sent file data")),
        };
        let v1 = sharing::Frame::decode(data.as_slice())?.v1.unwrap_or_default();
        match v1.r#type.and_then(|t| sharing::v1_frame::FrameType::try_from(t).ok()) {
            Some(sharing::v1_frame::FrameType::PairedKeyEncryption) => conn.send_sharing(&paired_key_result()).await?,
            Some(sharing::v1_frame::FrameType::PairedKeyResult) => {
                conn.send_sharing(&introduction(&files)).await?;
            }
            Some(sharing::v1_frame::FrameType::Response) => {
                let status = v1.connection_response.and_then(|r| r.status).unwrap_or(0);
                if status != sharing::connection_response_frame::Status::Accept as i32 {
                    return Ok(SendOutcome::Rejected(status));
                }
                break;
            }
            other => log::debug!("ignoring sharing frame {other:?}"),
        }
    }

    for (id, path, _, size) in &files {
        let mut file = tokio::fs::File::open(path).await?;
        let mut offset = 0;
        let mut buf = vec![0; CHUNK];
        loop {
            let n = file.read(&mut buf).await?;
            if n == 0 {
                break;
            }
            conn.send_chunk(*id, PayloadType::File, *size, offset, buf[..n].to_vec(), false).await?;
            offset += i64::try_from(n).map_err(|_| Error::Protocol("chunk too large"))?;
        }
        if hostile.extra_bytes > 0 {
            conn.send_chunk(*id, PayloadType::File, *size, offset, vec![0; hostile.extra_bytes], false).await?;
            offset += i64::try_from(hostile.extra_bytes).map_err(|_| Error::Protocol("chunk too large"))?;
        }
        conn.send_chunk(*id, PayloadType::File, *size, offset, Vec::new(), true).await?;
    }
    // The receiver disconnects once every file is in place.
    while !matches!(conn.recv().await?, Incoming::Disconnected) {}
    Ok(SendOutcome::Sent)
}

fn introduction(files: &[(i64, &PathBuf, String, i64)]) -> sharing::Frame {
    sharing_frame(sharing::v1_frame::FrameType::Introduction, |v1| {
        v1.introduction = Some(IntroductionFrame {
            file_metadata: files
                .iter()
                .map(|(id, _, name, size)| FileMetadata {
                    name: Some(name.clone()),
                    payload_id: Some(*id),
                    size: Some(*size),
                    id: Some(*id),
                    ..FileMetadata::default()
                })
                .collect(),
            ..IntroductionFrame::default()
        });
    })
}
