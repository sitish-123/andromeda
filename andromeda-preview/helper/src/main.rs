//! `andromeda-preview-helper <pdf|text> <path>`, started by `andromeda-preview` with a `SOCK_SEQPACKET` socket
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
        eprintln!("usage: andromeda-preview-helper <pdf|text> <path>");
        return ExitCode::from(2);
    };
    let sock = socket();
    if rustix::fs::fstat(&sock).is_err() {
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
    if let Err(e) = sandbox::enter() {
        eprintln!("andromeda-preview-helper: sandbox: {e}");
        return ExitCode::from(1);
    }
    let source = match kind.as_str() {
        "text" => Source::Text(file),
        "pdf" => if let Ok(pdf) = decode::Pdf::open(file) { Source::Pdf(pdf) } else {
            eprintln!("andromeda-preview-helper: not a readable PDF");
            return ExitCode::from(1);
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
