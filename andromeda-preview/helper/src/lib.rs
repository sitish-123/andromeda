//! Per-file decoder for `andromeda-preview` (docs/search.md §5). It parses untrusted files, so it runs
//! confined (see [`sandbox`]) and talks to the window only through the socket on fd 3.

pub mod annotate;
pub mod decode;
pub mod font;
pub mod media;
pub mod proto;
pub mod sandbox;

use proto::{
    ERR_BAD_REQUEST, KIND_TEXT, Msg, OP_ANNOT_ADD, OP_AUDIO, OP_COVER, OP_TAGS, OP_ANNOT_REMOVE, OP_FIND, OP_SAVE, OP_FRAME, OP_INFO, OP_INFO_REPLY, OP_RELEASE, OP_RENDER, OP_SELECT, OP_TEXT,
};
use rustix::fd::{AsFd, BorrowedFd};
use std::fs::File;

pub enum Source {
    Text(File),
    Pdf(decode::Pdf),
    Media(media::Media),
    Font(font::Font),
}

/// Answers requests until the window closes the socket.
///
/// # Errors
/// A socket error.
pub fn serve(sock: BorrowedFd<'_>, mut source: Source) -> std::io::Result<()> {
    while let Some((req, payload, req_fd)) = proto::recv_with(sock)? {
        if let Source::Media(m) = &mut source {
            match req.op {
                OP_AUDIO => match m.start_audio(req_fd) {
                    Ok(msg) => proto::send(sock, &msg, None)?,
                    Err(code) => proto::send(sock, &Msg::error(code), None)?,
                },
                proto::OP_PEAKS => match m.peaks(req.a) {
                    Ok((msg, fd)) => proto::send(sock, &msg, Some(fd.as_fd()))?,
                    Err(code) => proto::send(sock, &Msg::error(code), None)?,
                },
                OP_TAGS | OP_COVER => match if req.op == OP_TAGS { m.tag_text() } else { m.cover() } {
                    Ok((msg, fd)) => proto::send(sock, &msg, Some(fd.as_fd()))?,
                    Err(code) => proto::send(sock, &Msg::error(code), None)?,
                },
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
            (Source::Pdf(pdf), OP_FIND) => pdf.find(req.a, &payload),
            (Source::Pdf(pdf), OP_SELECT) => pdf.select(req.a, req.b, &payload),
            (Source::Pdf(pdf), OP_ANNOT_ADD) => pdf.annotate(req.a, req.b, req.c, req_fd).map(|m| (m, None)),
            (Source::Pdf(pdf), OP_ANNOT_REMOVE) => pdf.remove_annot(req.b).map(|m| (m, None)),
            (Source::Font(font), OP_RENDER) => font.specimen(req.b, req.c, req.d, req.e).map(|(m, fd)| (m, Some(fd))),
            (Source::Pdf(pdf), OP_SAVE) => pdf.save(req_fd).map(|m| (m, None)),
            _ => Err(ERR_BAD_REQUEST),
        };
        match reply {
            Ok((msg, fd)) => proto::send(sock, &msg, fd.as_ref().map(AsFd::as_fd))?,
            Err(code) => proto::send(sock, &Msg::error(code), None)?,
        }
    }
    Ok(())
}
