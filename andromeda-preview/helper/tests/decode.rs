use andromeda_preview_helper::proto::{
    self, ERR_BINARY, ERR_RANGE, KIND_PDF, KIND_TEXT, Msg, OP_INFO, OP_INFO_REPLY, OP_PAGE, OP_RENDER, OP_TEXT,
    OP_TEXT_REPLY,
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
