//! Sending to a LocalSend peer, and registering with one that announced itself: HTTPS pinned to the fingerprint it
//! announced.

use std::collections::BTreeMap;
use std::fs::File;
use std::net::{IpAddr, SocketAddr};
use std::os::unix::fs::FileExt;
use std::time::Duration;

use anyhow::{Context, bail};
use link_core::transfer::{CONSENT_TIMEOUT, Status};
use ring::digest;
use rustls::pki_types::ServerName;
use tokio::io::{AsyncWriteExt, BufReader};
use tokio::net::TcpStream;
use tokio_rustls::client::TlsStream;

use super::http::{self, MAX_JSON};
use super::tls;
use super::wire::{FileInfo, Info, PrepareUpload, Prepared};

const CONNECT_TIMEOUT: Duration = Duration::from_secs(5);
/// Between two writes of an upload; the receiver reads as fast as its disk allows.
const WRITE_TIMEOUT: Duration = Duration::from_secs(30);
const CHUNK: usize = 256 * 1024;

async fn connect(peer: &Info, address: IpAddr) -> anyhow::Result<TlsStream<TcpStream>> {
    let socket = SocketAddr::new(address, peer.port);
    let tcp = tokio::time::timeout(CONNECT_TIMEOUT, TcpStream::connect(socket)).await??;
    let connector = tokio_rustls::TlsConnector::from(tls::client(&peer.fingerprint)?);
    let name = ServerName::IpAddress(address.into());
    Ok(tokio::time::timeout(CONNECT_TIMEOUT, connector.connect(name, tcp)).await??)
}

/// POSTs a JSON body and returns the status and body of the answer, waiting up to `wait` for it.
async fn post_json(
    peer: &Info,
    address: IpAddr,
    target: &str,
    body: &[u8],
    wait: Duration,
) -> anyhow::Result<(u16, Vec<u8>)> {
    let mut stream = connect(peer, address).await?;
    let host = SocketAddr::new(address, peer.port).to_string();
    http::write_request(&mut stream, target, &host, "application/json", body.len() as u64).await?;
    stream.write_all(body).await?;
    stream.flush().await?;
    let mut stream = BufReader::new(stream);
    let (head, mut answer) = tokio::time::timeout(wait, http::read_response(&mut stream)).await??;
    let bytes = tokio::time::timeout(wait, answer.read_all(&mut stream, MAX_JSON)).await??;
    Ok((head.status, bytes))
}

pub async fn register(own: &Info, peer: &Info, address: IpAddr) -> anyhow::Result<()> {
    let body = serde_json::to_vec(own)?;
    let (status, _) = post_json(peer, address, "/api/localsend/v2/register", &body, CONNECT_TIMEOUT).await?;
    if status != 200 {
        bail!("register answered {status}");
    }
    Ok(())
}

/// Offers the files, waits for the peer's answer, then uploads each accepted one. `progress` gets the bytes sent.
pub async fn send(
    own: &Info,
    peer: &Info,
    address: IpAddr,
    files: Vec<(File, u64, String, String)>,
    progress: impl Fn(u64),
) -> Status {
    match try_send(own, peer, address, &files, &progress).await {
        Ok(status) => status,
        Err(error) => {
            log::info!("LocalSend: sending to {:?}: {error:#}", peer.alias);
            Status::Failed
        }
    }
}

async fn try_send(
    own: &Info,
    peer: &Info,
    address: IpAddr,
    files: &[(File, u64, String, String)],
    progress: &impl Fn(u64),
) -> anyhow::Result<Status> {
    let mut offered = BTreeMap::new();
    for (index, (file, size, name, mime)) in files.iter().enumerate() {
        let id = index.to_string();
        let sha256 = Some(hash(file, *size).await?);
        let info = FileInfo { id: id.clone(), file_name: name.clone(), size: *size, file_type: mime.clone(), sha256 };
        offered.insert(id, info);
    }
    let request = serde_json::to_vec(&PrepareUpload { info: own.clone(), files: offered })?;
    let wait = CONSENT_TIMEOUT + CONNECT_TIMEOUT;
    let (status, body) = post_json(peer, address, "/api/localsend/v2/prepare-upload", &request, wait).await?;
    let prepared: Prepared = match status {
        200 => serde_json::from_slice(&body).context("the prepare-upload answer")?,
        204 => return Ok(Status::Done),
        403 => return Ok(Status::Declined),
        409 | 429 => return Ok(Status::Busy),
        other => bail!("prepare-upload answered {other}"),
    };
    let mut sent = 0;
    for (id, token) in &prepared.files {
        let Some((file, size, _, _)) = id.parse::<usize>().ok().and_then(|index| files.get(index)) else { continue };
        let target = format!(
            "/api/localsend/v2/upload?sessionId={}&fileId={}&token={}",
            http::encode(&prepared.session_id),
            http::encode(id),
            http::encode(token)
        );
        upload(peer, address, &target, file, *size, &mut sent, progress).await?;
    }
    Ok(Status::Done)
}

async fn upload(
    peer: &Info,
    address: IpAddr,
    target: &str,
    file: &File,
    size: u64,
    sent: &mut u64,
    progress: &impl Fn(u64),
) -> anyhow::Result<()> {
    let mut stream = connect(peer, address).await?;
    let host = SocketAddr::new(address, peer.port).to_string();
    http::write_request(&mut stream, target, &host, "application/octet-stream", size).await?;
    let mut buf = vec![0; CHUNK];
    let mut offset = 0;
    while offset < size {
        let want = usize::try_from(size - offset).map_or(CHUNK, |left| left.min(CHUNK));
        let read = file.read_at(&mut buf[..want], offset)?;
        if read == 0 {
            bail!("the file shrank while it was sent");
        }
        tokio::time::timeout(WRITE_TIMEOUT, stream.write_all(&buf[..read])).await??;
        offset += read as u64;
        *sent += read as u64;
        progress(*sent);
    }
    stream.flush().await?;
    let mut stream = BufReader::new(stream);
    let (head, _) = tokio::time::timeout(WRITE_TIMEOUT, http::read_response(&mut stream)).await??;
    if head.status != 200 {
        bail!("upload answered {}", head.status);
    }
    Ok(())
}

async fn hash(file: &File, size: u64) -> anyhow::Result<String> {
    let mut context = digest::Context::new(&digest::SHA256);
    let mut buf = vec![0; CHUNK];
    let mut offset = 0;
    while offset < size {
        let read = file.read_at(&mut buf, offset)?;
        if read == 0 {
            break;
        }
        context.update(&buf[..read]);
        offset += read as u64;
        tokio::task::yield_now().await;
    }
    Ok(super::hex(context.finish().as_ref()))
}
