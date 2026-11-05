use andromeda_preview_helper::proto::{
    self, ERR_BINARY, ERR_RANGE, KIND_PDF, KIND_TEXT, Msg, OP_FIND, OP_FIND_REPLY, OP_INFO, OP_INFO_REPLY, OP_PAGE,
    OP_RENDER, OP_SELECT, OP_SELECT_REPLY, OP_TEXT, OP_TEXT_REPLY, SELECT_WORD,
};
use command_fds::{CommandFdExt, FdMapping};
use rustix::fd::{AsFd, OwnedFd};
use rustix::net::{AddressFamily, SocketFlags, SocketType};
use std::io::{Read, Seek, Write};
use std::process::{Child, Command};

struct Helper {
    sock: OwnedFd,
    child: Child,
}

impl Helper {
    fn start(kind: &str, path: &std::path::Path) -> Self {
        let (ours, theirs) =
            rustix::net::socketpair(AddressFamily::UNIX, SocketType::SEQPACKET, SocketFlags::CLOEXEC, None).unwrap();
        let mut cmd = Command::new(env!("CARGO_BIN_EXE_andromeda-preview-helper"));
        cmd.arg(kind).arg(path).env_clear();
        cmd.fd_mappings(vec![FdMapping { parent_fd: theirs, child_fd: 3 }]).unwrap();
        Self { sock: ours, child: cmd.spawn().unwrap() }
    }

    fn ask(&self, msg: Msg) -> (Msg, Option<OwnedFd>) {
        proto::send(self.sock.as_fd(), &msg, None).unwrap();
        proto::recv(self.sock.as_fd()).unwrap().expect("helper closed the socket")
    }

    fn ask_with(&self, msg: Msg, payload: &[u8]) -> (Msg, Option<OwnedFd>) {
        proto::send_with(self.sock.as_fd(), &msg, payload, &[]).unwrap();
        proto::recv(self.sock.as_fd()).unwrap().expect("helper closed the socket")
    }
}

impl Drop for Helper {
    fn drop(&mut self) {
        let _ = self.child.kill();
        let _ = self.child.wait();
    }
}

fn read_fd(fd: OwnedFd) -> Vec<u8> {
    let mut file = std::fs::File::from(fd);
    file.rewind().unwrap();
    let mut out = Vec::new();
    file.read_to_end(&mut out).unwrap();
    out
}

fn temp_with(bytes: &[u8]) -> tempfile::NamedTempFile {
    let mut f = tempfile::NamedTempFile::new().unwrap();
    f.write_all(bytes).unwrap();
    f
}

#[test]
fn text_file_returns_its_text() {
    let file = temp_with("héllo\nworld\n".as_bytes());
    let helper = Helper::start("text", file.path());
    assert_eq!(helper.ask(Msg::new(OP_INFO, 0, 0, 0, 0)).0.a, KIND_TEXT);
    let (reply, fd) = helper.ask(Msg::new(OP_TEXT, 0, 0, 0, 0));
    assert_eq!(reply.op, OP_TEXT_REPLY);
    assert_eq!(reply.b, 0);
    assert_eq!(read_fd(fd.unwrap()), "héllo\nworld\n".as_bytes());
}

#[test]
fn long_text_is_cut_at_one_mebibyte_on_a_char_boundary() {
    // 3-byte characters: the 1 MiB limit falls inside one.
    let body = "€".repeat(400_000);
    let file = temp_with(body.as_bytes());
    let helper = Helper::start("text", file.path());
    let (reply, fd) = helper.ask(Msg::new(OP_TEXT, 0, 0, 0, 0));
    assert_eq!(reply.b, 1);
    let text = String::from_utf8(read_fd(fd.unwrap())).unwrap();
    assert_eq!(text.len(), (1 << 20) / 3 * 3);
    assert!(text.chars().all(|c| c == '€'));
}

#[test]
fn invalid_utf8_reply_stays_within_one_mebibyte() {
    let file = temp_with(&vec![0xff; 1 << 20]);
    let helper = Helper::start("text", file.path());
    let (reply, fd) = helper.ask(Msg::new(OP_TEXT, 0, 0, 0, 0));
    assert!(reply.a as usize <= 1 << 20);
    assert_eq!(reply.b, 1);
    assert_eq!(read_fd(fd.unwrap()).len(), reply.a as usize);
}

#[test]
fn second_text_request_returns_the_same_text() {
    let file = temp_with(b"again\n");
    let helper = Helper::start("text", file.path());
    let (_, first) = helper.ask(Msg::new(OP_TEXT, 0, 0, 0, 0));
    let (_, second) = helper.ask(Msg::new(OP_TEXT, 0, 0, 0, 0));
    assert_eq!(read_fd(first.unwrap()), read_fd(second.unwrap()));
}

#[test]
fn binary_file_is_refused_as_text() {
    let file = temp_with(&[0x7f, b'E', b'L', b'F', 0, 0, 1]);
    let helper = Helper::start("text", file.path());
    let (reply, fd) = helper.ask(Msg::new(OP_TEXT, 0, 0, 0, 0));
    assert_eq!((reply.op, reply.a), (proto::OP_ERROR, ERR_BINARY));
    assert!(fd.is_none());
}

/// One 200 x 100 pt page, left half black.
fn sample_pdf() -> tempfile::NamedTempFile {
    let file = tempfile::Builder::new().suffix(".pdf").tempfile().unwrap();
    let surface = cairo::PdfSurface::new(200.0, 100.0, file.path()).unwrap();
    let cr = cairo::Context::new(&surface).unwrap();
    cr.rectangle(0.0, 0.0, 100.0, 100.0);
    cr.set_source_rgb(0.0, 0.0, 0.0);
    cr.fill().unwrap();
    drop(cr);
    surface.finish();
    file
}

#[test]
fn pdf_page_renders_at_the_asked_width() {
    let file = sample_pdf();
    let helper = Helper::start("pdf", file.path());
    let (info, _) = helper.ask(Msg::new(OP_INFO, 0, 0, 0, 0));
    assert_eq!((info.op, info.a, info.b, info.c, info.d), (OP_INFO_REPLY, KIND_PDF, 1, 20000, 10000));

    let (page, fd) = helper.ask(Msg::new(OP_RENDER, 0, 400, 4096, 0));
    assert_eq!((page.op, page.b, page.c), (OP_PAGE, 400, 200));
    let stride = page.d as usize;
    let pixels = read_fd(fd.unwrap());
    assert!(pixels.len() >= stride * 200);
    let px = |x: usize, y: usize| u32::from_ne_bytes(pixels[y * stride + x * 4..][..4].try_into().unwrap());
    assert_eq!(px(50, 100), 0xff00_0000);
    assert_eq!(px(350, 100), 0xffff_ffff);
}

#[test]
fn pdf_page_out_of_range_is_an_error() {
    let file = sample_pdf();
    let helper = Helper::start("pdf", file.path());
    let (reply, _) = helper.ask(Msg::new(OP_RENDER, 5, 400, 4096, 0));
    assert_eq!((reply.op, reply.a), (proto::OP_ERROR, ERR_RANGE));
}

#[test]
fn broken_pdf_makes_the_helper_exit() {
    let file = temp_with(b"%PDF-1.7\nnot really");
    let mut helper = Helper::start("pdf", file.path());
    let status = helper.child.wait().unwrap();
    assert!(!status.success());
}

/// One 300 x 100 pt page with "Hello Quick Look" on a baseline 50 pt from the top.
fn text_pdf() -> tempfile::NamedTempFile {
    let file = tempfile::Builder::new().suffix(".pdf").tempfile().unwrap();
    let surface = cairo::PdfSurface::new(300.0, 100.0, file.path()).unwrap();
    let cr = cairo::Context::new(&surface).unwrap();
    cr.set_font_size(20.0);
    cr.move_to(20.0, 50.0);
    cr.show_text("Hello Quick Look").unwrap();
    drop(cr);
    surface.finish();
    file
}

fn rects(bytes: &[u8], count: usize) -> Vec<[f32; 4]> {
    bytes[..count * 16]
        .as_chunks::<16>()
        .0
        .iter()
        .map(|r| std::array::from_fn(|i| f32::from_le_bytes(r[i * 4..i * 4 + 4].try_into().unwrap())))
        .collect()
}

#[test]
fn find_returns_the_word_on_its_line_with_a_top_left_origin() {
    let file = text_pdf();
    let helper = Helper::start("pdf", file.path());
    let (reply, fd) = helper.ask_with(Msg::new(OP_FIND, 0, 0, 0, 0), b"quick");
    assert_eq!((reply.op, reply.a, reply.b), (OP_FIND_REPLY, 0, 1));
    let [x0, y0, x1, y1] = rects(&read_fd(fd.unwrap()), 1)[0];
    // The word sits right of "Hello " and its box spans the baseline at y = 50.
    assert!(x0 > 60.0 && x1 > x0 && x1 < 200.0, "x {x0}..{x1}");
    assert!(y0 > 25.0 && y0 < 50.0 && y1 > 45.0 && y1 < 60.0, "y {y0}..{y1}");

    let (none, fd) = helper.ask_with(Msg::new(OP_FIND, 0, 0, 0, 0), b"absent");
    assert_eq!((none.op, none.b), (OP_FIND_REPLY, 0));
    assert!(fd.is_none());
}

#[test]
fn select_over_the_line_returns_its_text_and_rects() {
    let file = text_pdf();
    let helper = Helper::start("pdf", file.path());
    let area: Vec<u8> = [0.0f32, 0.0, 300.0, 100.0].iter().flat_map(|v| v.to_le_bytes()).collect();
    let (reply, fd) = helper.ask_with(Msg::new(OP_SELECT, 0, SELECT_WORD, 0, 0), &area);
    assert_eq!(reply.op, OP_SELECT_REPLY);
    let bytes = read_fd(fd.unwrap());
    let count = reply.b as usize;
    assert!(count >= 1);
    let text = std::str::from_utf8(&bytes[count * 16..][..reply.a as usize]).unwrap();
    assert_eq!(text.trim(), "Hello Quick Look");
    assert!(rects(&bytes, count).iter().all(|[x0, y0, x1, y1]| x1 > x0 && y1 > y0 && *y0 < 50.0));
}

