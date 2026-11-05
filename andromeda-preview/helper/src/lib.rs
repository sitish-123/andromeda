//! Per-file decoder for `andromeda-preview` (docs/search.md §5). It parses untrusted files, so it runs
//! confined (see [`sandbox`]) and talks to the window only through the socket on fd 3.

pub mod decode;
pub mod media;
pub mod proto;
pub mod sandbox;

use proto::{ERR_BAD_REQUEST, KIND_TEXT, Msg, OP_FRAME, OP_INFO, OP_INFO_REPLY, OP_RELEASE, OP_RENDER, OP_TEXT};
use rustix::fd::{AsFd, BorrowedFd};
use std::fs::File;

pub enum Source {
    Text(File),
    Pdf(decode::Pdf),
    Media(media::Media),
}

/// Answers requests until the window closes the socket.
///
/// # Errors
/// A socket error.
pub fn serve(sock: BorrowedFd<'_>, mut source: Source) -> std::io::Result<()> {
    while let Some((req, _)) = proto::recv(sock)? {
        if let Source::Media(m) = &mut source {
            match req.op {
                // Releases get no reply: the window sends them as it drops frames.
                OP_RELEASE => m.release(req.a),
                OP_INFO => proto::send(sock, &m.info(), None)?,
                OP_FRAME => match m.next_frame() {
                    Ok(f) => {
                        let fds: Vec<_> = f.fds.iter().map(AsFd::as_fd).collect();
                        proto::send_with(sock, &f.msg, &f.payload, &fds)?;
                    }
                    Err(code) => proto::send(sock, &Msg::error(code), None)?,
                },
                _ => proto::send(sock, &Msg::error(ERR_BAD_REQUEST), None)?,
            }
            continue;
        }
        let reply = match (&mut source, req.op) {
            (Source::Text(_), OP_INFO) => Ok((Msg::new(OP_INFO_REPLY, KIND_TEXT, 1, 0, 0), None)),
            (Source::Text(file), OP_TEXT) => decode::text_reply(file).map(|(m, fd)| (m, Some(fd))),
            (Source::Pdf(pdf), OP_INFO) => Ok((pdf.info(), None)),
            (Source::Pdf(pdf), OP_RENDER) => pdf.render(req.a, req.b, req.c).map(|(m, fd)| (m, Some(fd))),
            _ => Err(ERR_BAD_REQUEST),
        };
        match reply {
            Ok((msg, fd)) => proto::send(sock, &msg, fd.as_ref().map(AsFd::as_fd))?,
            Err(code) => proto::send(sock, &Msg::error(code), None)?,
        }
    }
    Ok(())
}
