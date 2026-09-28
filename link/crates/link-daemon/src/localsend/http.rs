//! Just enough HTTP/1.1 for LocalSend: one request per connection, heads parsed by `httparse`, bodies by
//! Content-Length or chunked. Everything read here is untrusted and bounded.

use std::collections::HashMap;

use anyhow::{Context, bail};
use tokio::io::{AsyncBufReadExt, AsyncRead, AsyncReadExt, AsyncWrite, AsyncWriteExt, BufReader};

const MAX_HEAD: usize = 16 * 1024;
const MAX_HEADERS: usize = 64;
/// JSON bodies; file bodies stream and are bounded by the file's size instead.
pub const MAX_JSON: u64 = 1 << 20;

pub struct Head {
    pub method: String,
    pub path: String,
    pub query: HashMap<String, String>,
    pub status: u16,
    body: Framing,
}

#[derive(Clone, Copy)]
enum Framing {
    Length(u64),
    Chunked,
    /// A response without a length: the body ends with the connection.
    Close,
}

/// Reads a body as it arrives.
pub struct Body {
    framing: Framing,
    left: u64,
    done: bool,
}

async fn read_head_bytes<S: AsyncRead + Unpin>(stream: &mut BufReader<S>) -> anyhow::Result<Vec<u8>> {
    let mut head = Vec::new();
    while !head.ends_with(b"\r\n\r\n") {
        let before = head.len();
        stream.read_until(b'\n', &mut head).await?;
        if head.len() == before {
            bail!("the connection ended inside the head");
        }
        if head.len() > MAX_HEAD {
            bail!("a head over 16 KiB");
        }
    }
    Ok(head)
}

fn framing(headers: &[httparse::Header<'_>], response: bool) -> anyhow::Result<Framing> {
    let value =
        |name: &str| headers.iter().find(|header| header.name.eq_ignore_ascii_case(name)).map(|header| header.value);
    if value("transfer-encoding").is_some_and(|value| value.eq_ignore_ascii_case(b"chunked")) {
        return Ok(Framing::Chunked);
    }
    match value("content-length") {
        Some(length) => Ok(Framing::Length(std::str::from_utf8(length)?.trim().parse().context("content-length")?)),
        None if response => Ok(Framing::Close),
        None => Ok(Framing::Length(0)),
    }
}

pub async fn read_request<S: AsyncRead + Unpin>(stream: &mut BufReader<S>) -> anyhow::Result<(Head, Body)> {
    let bytes = read_head_bytes(stream).await?;
    let mut headers = [httparse::EMPTY_HEADER; MAX_HEADERS];
    let mut request = httparse::Request::new(&mut headers);
    if !request.parse(&bytes)?.is_complete() {
        bail!("an incomplete request head");
    }
    let target = request.path.context("no path")?;
    let (path, query) = target.split_once('?').unwrap_or((target, ""));
    let head = Head {
        method: request.method.context("no method")?.to_owned(),
        path: path.to_owned(),
        query: parse_query(query),
        status: 0,
        body: framing(request.headers, false)?,
    };
    let body = Body::new(head.body);
    Ok((head, body))
}

pub async fn read_response<S: AsyncRead + Unpin>(stream: &mut BufReader<S>) -> anyhow::Result<(Head, Body)> {
    let bytes = read_head_bytes(stream).await?;
    let mut headers = [httparse::EMPTY_HEADER; MAX_HEADERS];
    let mut response = httparse::Response::new(&mut headers);
    if !response.parse(&bytes)?.is_complete() {
        bail!("an incomplete response head");
    }
    let head = Head {
        method: String::new(),
        path: String::new(),
        query: HashMap::new(),
        status: response.code.context("no status")?,
        body: framing(response.headers, true)?,
    };
    let body = Body::new(head.body);
    Ok((head, body))
}

fn parse_query(query: &str) -> HashMap<String, String> {
    query.split('&').filter_map(|pair| pair.split_once('=')).map(|(key, value)| (decode(key), decode(value))).collect()
}

fn decode(text: &str) -> String {
    let bytes = text.as_bytes();
    let mut out = Vec::with_capacity(bytes.len());
    let mut index = 0;
    while index < bytes.len() {
        let hex =
            bytes.get(index + 1..index + 3).and_then(|hex| u8::from_str_radix(std::str::from_utf8(hex).ok()?, 16).ok());
        match (bytes[index], hex) {
            (b'%', Some(byte)) => {
                out.push(byte);
                index += 3;
            }
            (b'+', _) => {
                out.push(b' ');
                index += 1;
            }
            (byte, _) => {
                out.push(byte);
                index += 1;
            }
        }
    }
    String::from_utf8_lossy(&out).into_owned()
}

pub fn encode(text: &str) -> String {
    text.bytes()
        .map(|byte| match byte {
            b'A'..=b'Z' | b'a'..=b'z' | b'0'..=b'9' | b'-' | b'_' | b'.' | b'~' => char::from(byte).to_string(),
            _ => format!("%{byte:02X}"),
        })
        .collect()
}

impl Body {
    fn new(framing: Framing) -> Self {
        let left = if let Framing::Length(length) = framing { length } else { 0 };
        Self { framing, left, done: matches!(framing, Framing::Length(0)) }
    }

    /// The announced length, when there is one.
    pub fn length(&self) -> Option<u64> {
        if let Framing::Length(length) = self.framing { Some(length) } else { None }
    }

    /// The next bytes into `buf`; 0 at the end.
    pub async fn read<S: AsyncRead + Unpin>(
        &mut self,
        stream: &mut BufReader<S>,
        buf: &mut [u8],
    ) -> anyhow::Result<usize> {
        if self.done {
            return Ok(0);
        }
        if matches!(self.framing, Framing::Chunked) && self.left == 0 {
            self.left = chunk_size(stream).await?;
            if self.left == 0 {
                self.done = true;
                return Ok(0);
            }
        }
        let want = if matches!(self.framing, Framing::Close) {
            buf.len()
        } else {
            usize::try_from(self.left).map_or(buf.len(), |left| left.min(buf.len()))
        };
        let read = stream.read(&mut buf[..want]).await?;
        if read == 0 {
            if matches!(self.framing, Framing::Close) {
                self.done = true;
                return Ok(0);
            }
            bail!("the connection ended inside the body");
        }
        if !matches!(self.framing, Framing::Close) {
            self.left -= read as u64;
        }
        match self.framing {
            Framing::Length(_) if self.left == 0 => self.done = true,
            Framing::Chunked if self.left == 0 => {
                let mut crlf = [0; 2];
                stream.read_exact(&mut crlf).await?;
            }
            _ => {}
        }
        Ok(read)
    }

    /// The whole body, at most `max` bytes.
    pub async fn read_all<S: AsyncRead + Unpin>(
        &mut self,
        stream: &mut BufReader<S>,
        max: u64,
    ) -> anyhow::Result<Vec<u8>> {
        let mut out = Vec::new();
        let mut buf = vec![0; 16 * 1024];
        loop {
            let read = self.read(stream, &mut buf).await?;
            if read == 0 {
                return Ok(out);
            }
            out.extend_from_slice(&buf[..read]);
            if out.len() as u64 > max {
                bail!("a body over {max} bytes");
            }
        }
    }
}

/// A chunk-size line; trailers after the last chunk are skipped.
async fn chunk_size<S: AsyncRead + Unpin>(stream: &mut BufReader<S>) -> anyhow::Result<u64> {
    let mut line = String::new();
    stream.take(1024).read_line(&mut line).await?;
    let size = u64::from_str_radix(line.trim().split(';').next().unwrap_or_default(), 16).context("chunk size")?;
    if size == 0 {
        loop {
            let mut trailer = String::new();
            if stream.take(1024).read_line(&mut trailer).await? == 0 || trailer.trim().is_empty() {
                break;
            }
        }
    }
    Ok(size)
}

pub async fn write_response<S: AsyncWrite + Unpin>(
    stream: &mut S,
    status: u16,
    json: Option<&[u8]>,
) -> anyhow::Result<()> {
    let body = json.unwrap_or_default();
    let kind = if json.is_some() { "Content-Type: application/json\r\n" } else { "" };
    let head = format!(
        "HTTP/1.1 {status} {}\r\n{kind}Content-Length: {}\r\nConnection: close\r\n\r\n",
        reason(status),
        body.len()
    );
    stream.write_all(head.as_bytes()).await?;
    stream.write_all(body).await?;
    stream.flush().await?;
    Ok(())
}

/// Writes a request head; the caller writes exactly `length` body bytes after it.
pub async fn write_request<S: AsyncWrite + Unpin>(
    stream: &mut S,
    target: &str,
    host: &str,
    kind: &str,
    length: u64,
) -> anyhow::Result<()> {
    let head = format!(
        "POST {target} HTTP/1.1\r\nHost: {host}\r\nContent-Type: {kind}\r\nContent-Length: {length}\r\nConnection: close\r\n\r\n"
    );
    stream.write_all(head.as_bytes()).await?;
    Ok(())
}

fn reason(status: u16) -> &'static str {
    match status {
        200 => "OK",
        204 => "No Content",
        400 => "Bad Request",
        403 => "Forbidden",
        404 => "Not Found",
        409 => "Conflict",
        422 => "Unprocessable Entity",
        429 => "Too Many Requests",
        _ => "Internal Server Error",
    }
}
