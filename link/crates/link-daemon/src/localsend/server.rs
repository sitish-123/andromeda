//! The LocalSend HTTPS server: one request per connection, each connection a task of the actor.

use std::net::SocketAddr;
use std::sync::Arc;
use std::time::Duration;

use link_core::inbox::Part;
use link_core::proto::transfer::Budget;
use tokio::io::{AsyncRead, BufReader};
use tokio::net::{TcpListener, TcpStream};
use tokio::task::JoinSet;

use super::http::{self, Body};
use super::wire::{Announcement, Info, PrepareUpload};
use super::{Answer, LocalSendHandle, Received};

/// How long a peer may take to finish its TLS handshake and request head.
const STEP_TIMEOUT: Duration = Duration::from_secs(10);
const CHUNK: usize = 256 * 1024;
const PROGRESS_EVERY: u64 = 1 << 20;

pub async fn accept(
    listener: TcpListener,
    acceptor: tokio_rustls::TlsAcceptor,
    handle: LocalSendHandle,
    info: Arc<Vec<u8>>,
) {
    let mut connections = JoinSet::new();
    loop {
        tokio::select! {
            accepted = listener.accept() => match accepted {
                Ok((stream, from)) => {
                    let (acceptor, handle, info) = (acceptor.clone(), handle.clone(), info.clone());
                    connections.spawn(async move {
                        if let Err(error) = serve(stream, from, acceptor, handle, info).await {
                            log::info!("LocalSend: {from}: {error:#}");
                        }
                    });
                }
                Err(error) => {
                    log::warn!("LocalSend: accepting: {error}");
                    return;
                }
            },
            Some(_) = connections.join_next() => {}
        }
    }
}

async fn serve(
    stream: TcpStream,
    from: SocketAddr,
    acceptor: tokio_rustls::TlsAcceptor,
    handle: LocalSendHandle,
    info: Arc<Vec<u8>>,
) -> anyhow::Result<()> {
    let tls = tokio::time::timeout(STEP_TIMEOUT, acceptor.accept(stream)).await??;
    let (read, mut write) = tokio::io::split(tls);
    let mut read = BufReader::new(read);
    let (head, mut body) = tokio::time::timeout(STEP_TIMEOUT, http::read_request(&mut read)).await??;
    let query = |key: &str| head.query.get(key).cloned().unwrap_or_default();
    match (head.method.as_str(), head.path.as_str()) {
        ("POST" | "GET", "/api/localsend/v2/register" | "/api/localsend/v2/info") => {
            let bytes = body.read_all(&mut read, http::MAX_JSON).await?;
            // A register answers an announcement of ours, so its sender is a peer on the LAN.
            if let Ok(peer) = serde_json::from_slice::<Info>(&bytes) {
                handle.announced(Announcement { info: peer, announce: false }, from.ip()).await;
            }
            http::write_response(&mut write, 200, Some(&info)).await
        }
        ("POST", "/api/localsend/v2/prepare-upload") => {
            let bytes = body.read_all(&mut read, http::MAX_JSON).await?;
            let Ok(request) = serde_json::from_slice::<PrepareUpload>(&bytes) else {
                return http::write_response(&mut write, 400, None).await;
            };
            match handle.prepare(request).await {
                Answer::Accepted(prepared) => {
                    http::write_response(&mut write, 200, Some(&serde_json::to_vec(&prepared)?)).await
                }
                Answer::Status(status) => http::write_response(&mut write, status, None).await,
            }
        }
        ("POST", "/api/localsend/v2/upload") => {
            let (session, file) = (query("sessionId"), query("fileId"));
            let status = upload(&mut read, &mut body, &handle, &session, &file, &query("token")).await;
            http::write_response(&mut write, status, None).await
        }
        ("POST", "/api/localsend/v2/cancel") => {
            handle.cancel_session(query("sessionId")).await;
            http::write_response(&mut write, 200, None).await
        }
        _ => http::write_response(&mut write, 404, None).await,
    }
}

/// Streams one file into its part within the announced size and returns the HTTP status to answer with.
async fn upload<R: AsyncRead + Unpin>(
    read: &mut BufReader<R>,
    body: &mut Body,
    handle: &LocalSendHandle,
    session: &str,
    file: &str,
    token: &str,
) -> u16 {
    let ticket = match handle.upload(session.to_owned(), file.to_owned(), token.to_owned()).await {
        Ok(ticket) => ticket,
        Err(status) => return status,
    };
    let received = if body.length().is_some_and(|length| length != ticket.size) {
        Received::WrongSize
    } else {
        write_part(read, body, &ticket.part, ticket.size, handle, session).await
    };
    handle.uploaded(session.to_owned(), file.to_owned(), received).await
}

async fn write_part<R: AsyncRead + Unpin>(
    read: &mut BufReader<R>,
    body: &mut Body,
    part: &std::path::Path,
    size: u64,
    handle: &LocalSendHandle,
    session: &str,
) -> Received {
    let Ok(mut part) = Part::open(part, 0).await else { return Received::Failed };
    let mut budget = Budget::new(size);
    let mut buf = vec![0; CHUNK];
    let mut reported = 0;
    loop {
        let read = match body.read(read, &mut buf).await {
            Ok(0) => break,
            Ok(read) => read,
            Err(error) => {
                log::info!("LocalSend: session {session}: {error:#}");
                return Received::Failed;
            }
        };
        if budget.take(read).is_err() {
            return Received::WrongSize;
        }
        if part.write(&buf[..read]).is_err() {
            return Received::Failed;
        }
        if budget.offset() - reported >= PROGRESS_EVERY {
            reported = budget.offset();
            handle.session_progress(session, reported);
        }
    }
    if budget.finish().is_err() {
        return Received::WrongSize;
    }
    if part.sync().is_err() {
        return Received::Failed;
    }
    Received::Complete { digest: part.digest() }
}
