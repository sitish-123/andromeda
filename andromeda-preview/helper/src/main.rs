//! `andromeda-preview-helper <pdf|text|font|media|media-sw> <path>`, started by `andromeda-preview` with a `SOCK_SEQPACKET` socket
//! on fd 3, an empty environment apart from HOME and `XDG_CACHE_HOME`, and no other fds.
//! `andromeda-preview-helper thumbnail[-video|-font] <size> <input> <output.png>`: freedesktop thumbnailers for PDFs,
//! videos (a frame at 10 %) and fonts (data/andromeda-{pdf,video,font}.thumbnailer), sandboxed the same way once both
//! files are open.

use andromeda_preview_helper::{Source, decode, sandbox, serve};
use std::os::fd::{FromRawFd, OwnedFd};
use std::process::ExitCode;

const SOCKET_FD: i32 = 3;

#[allow(unsafe_code)]
fn socket() -> OwnedFd {
    // SAFETY: the parent passes fd 3 as our only socket; nothing else in this process owns it.
    unsafe { OwnedFd::from_raw_fd(SOCKET_FD) }
}

/// What a thumbnail mode makes: the PDF first page, a video frame, or a font's "Aa".
#[derive(Clone, Copy, PartialEq, Eq)]
enum Thumb {
    Pdf,
    Video,
    Font,
}

fn thumbnail(kind: Thumb, size: &str, input: &str, output: &str) -> ExitCode {
    let Ok(size) = size.parse::<u32>() else { return ExitCode::from(2) };
    let mut file = match std::fs::File::open(input) {
        Ok(file) => file,
        Err(e) => {
            eprintln!("andromeda-preview-helper: open: {e}");
            return ExitCode::from(1);
        }
    };
    // Before the sandbox: the font bytes (no reads after it), the GStreamer registry and pipeline.
    let font_bytes = if kind == Thumb::Font { andromeda_preview_helper::font::Font::read(&mut file).ok() } else { None };
    let pipeline = if kind == Thumb::Video {
        if andromeda_preview_helper::media::init().is_err() {
            return ExitCode::from(1);
        }
        Some(andromeda_preview_helper::media::Media::prepare(false))
    } else {
        None
    };
    // create_new: never write through a file or link already at the output path.
    let mut out = match std::fs::OpenOptions::new().write(true).create_new(true).open(output) {
        Ok(out) => std::io::BufWriter::new(out),
        Err(e) => {
            eprintln!("andromeda-preview-helper: output: {e}");
            return ExitCode::from(1);
        }
    };
    let profile = if kind == Thumb::Video { sandbox::Profile::Media } else { sandbox::Profile::Document };
    if let Err(e) = sandbox::enter(profile) {
        eprintln!("andromeda-preview-helper: sandbox: {e}");
        return ExitCode::from(1);
    }
    let made = match kind {
        Thumb::Pdf => decode::Pdf::open(file).and_then(|pdf| pdf.thumbnail_png(size, &mut out)),
        Thumb::Font => font_bytes
            .ok_or(0)
            .and_then(andromeda_preview_helper::font::Font::open)
            .and_then(|font| font.thumbnail_png(size, &mut out)),
        Thumb::Video => pipeline
            .ok_or(0)
            .and_then(|p| andromeda_preview_helper::media::Media::open(p, file))
            .and_then(|mut media| media.thumbnail_png(size, &mut out)),
    };
    match made.and_then(|()| std::io::Write::flush(&mut out).map_err(|_| 0)) {
        Ok(()) => ExitCode::SUCCESS,
        Err(code) => {
            eprintln!("andromeda-preview-helper: no thumbnail (error {code})");
            ExitCode::from(1)
        }
    }
}

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().collect();
    if let [_, mode, size, input, output] = args.as_slice() {
        let kind = match mode.as_str() {
            "thumbnail" => Some(Thumb::Pdf),
            "thumbnail-video" => Some(Thumb::Video),
            "thumbnail-font" => Some(Thumb::Font),
            _ => None,
        };
        if let Some(kind) = kind {
            return thumbnail(kind, size, input, output);
        }
    }
    let [_, kind, path] = args.as_slice() else {
        eprintln!("usage: andromeda-preview-helper <pdf|text|font|media|media-sw> <path>");
        return ExitCode::from(2);
    };
    let sock = socket();
    if rustix::fs::fstat(&sock).is_err() {
        // Not ours to close: fd 3 is not open, and dropping the OwnedFd would abort.
        std::mem::forget(sock);
        eprintln!("andromeda-preview-helper: no socket on fd 3");
        return ExitCode::from(2);
    }
    let mut file = match std::fs::File::open(path) {
        Ok(file) => file,
        Err(e) => {
            eprintln!("andromeda-preview-helper: open: {e}");
            return ExitCode::from(1);
        }
    };
    // A font is read whole before the sandbox; FreeType parses it after.
    let font_bytes = if kind == "font" { andromeda_preview_helper::font::Font::read(&mut file).ok() } else { None };
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
        "font" => match font_bytes.ok_or(0).and_then(andromeda_preview_helper::font::Font::open) {
            Ok(font) => Source::Font(font),
            Err(_) => {
                eprintln!("andromeda-preview-helper: not a readable font");
                return ExitCode::from(1);
            }
        },
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
