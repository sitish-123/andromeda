//! `andromeda-preview-helper <pdf|text|media|media-sw> <path>`, started by `andromeda-preview` with a `SOCK_SEQPACKET` socket
//! on fd 3, an empty environment apart from HOME and `XDG_CACHE_HOME`, and no other fds.

use andromeda_preview_helper::{Source, decode, sandbox, serve};
use std::os::fd::{FromRawFd, OwnedFd};
use std::process::ExitCode;

const SOCKET_FD: i32 = 3;

#[allow(unsafe_code)]
fn socket() -> OwnedFd {
    // SAFETY: the parent passes fd 3 as our only socket; nothing else in this process owns it.
    unsafe { OwnedFd::from_raw_fd(SOCKET_FD) }
}

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().collect();
    let [_, kind, path] = args.as_slice() else {
        eprintln!("usage: andromeda-preview-helper <pdf|text|media|media-sw> <path>");
        return ExitCode::from(2);
    };
    let sock = socket();
    if rustix::fs::fstat(&sock).is_err() {
        // Not ours to close: fd 3 is not open, and dropping the OwnedFd would abort.
        std::mem::forget(sock);
        eprintln!("andromeda-preview-helper: no socket on fd 3");
        return ExitCode::from(2);
    }
    let file = match std::fs::File::open(path) {
        Ok(file) => file,
        Err(e) => {
            eprintln!("andromeda-preview-helper: open: {e}");
            return ExitCode::from(1);
        }
    };
    let profile = if kind == "media" || kind == "media-sw" { sandbox::Profile::Media } else { sandbox::Profile::Document };
    let mut pipeline = None;
    if profile == sandbox::Profile::Media {
        if let Err(e) = andromeda_preview_helper::media::init() {
            eprintln!("andromeda-preview-helper: gstreamer: {e}");
            return ExitCode::from(1);
        }
        pipeline = Some(andromeda_preview_helper::media::Media::prepare(kind == "media"));
    }
    if let Err(e) = sandbox::enter(profile) {
        eprintln!("andromeda-preview-helper: sandbox: {e}");
        return ExitCode::from(1);
    }
    let source = match kind.as_str() {
        "text" => Source::Text(file),
        "pdf" => if let Ok(pdf) = decode::Pdf::open(file) { Source::Pdf(pdf) } else {
            eprintln!("andromeda-preview-helper: not a readable PDF");
            return ExitCode::from(1);
        },
        "media" | "media-sw" => match pipeline.map(|p| andromeda_preview_helper::media::Media::open(p, file)).unwrap_or(Err(0)) {
            Ok(media) => Source::Media(media),
            Err(_) => {
                eprintln!("andromeda-preview-helper: no decodable video");
                return ExitCode::from(1);
            }
        },
        _ => return ExitCode::from(2),
    };
    match serve(rustix::fd::AsFd::as_fd(&sock), source) {
        Ok(()) => ExitCode::SUCCESS,
        Err(e) => {
            eprintln!("andromeda-preview-helper: {e}");
            ExitCode::from(1)
        }
    }
}
