use andromeda_preview_helper::annotate::{ANNOT_HIGHLIGHT, ANNOT_INK, ANNOT_NOTE, ANNOT_STAMP};
use andromeda_preview_helper::proto::{self, Msg, OP_ANNOT_ADD, OP_ANNOT_REMOVE, OP_ANNOT_REPLY, OP_SAVE, OP_SAVED};
use command_fds::{CommandFdExt, FdMapping};
use rustix::fd::{AsFd, OwnedFd};
use rustix::net::{AddressFamily, SocketFlags, SocketType};
use std::io::Write;
use std::process::{Child, Command};

struct Helper {
    sock: OwnedFd,
    child: Child,
}

impl Helper {
    fn start(path: &std::path::Path) -> Self {
        let (ours, theirs) =
            rustix::net::socketpair(AddressFamily::UNIX, SocketType::SEQPACKET, SocketFlags::CLOEXEC, None).unwrap();
        let mut cmd = Command::new(env!("CARGO_BIN_EXE_andromeda-preview-helper"));
        cmd.arg("pdf").arg(path).env_clear();
        cmd.fd_mappings(vec![FdMapping { parent_fd: theirs, child_fd: 3 }]).unwrap();
        Self { sock: ours, child: cmd.spawn().unwrap() }
    }

    fn ask(&self, msg: Msg, fd: Option<OwnedFd>) -> Msg {
        let fds: Vec<_> = fd.iter().map(AsFd::as_fd).collect();
        proto::send_with(self.sock.as_fd(), &msg, &[], &fds).unwrap();
        proto::recv(self.sock.as_fd()).unwrap().expect("helper closed the socket").0
    }
}

impl Drop for Helper {
    fn drop(&mut self) {
        let _ = self.child.kill();
        let _ = self.child.wait();
    }
}

fn body(bytes: &[u8]) -> OwnedFd {
    let mut f = tempfile::tempfile().unwrap();
    f.write_all(bytes).unwrap();
    OwnedFd::from(f)
}

fn f32s(values: &[f32]) -> Vec<u8> {
    values.iter().flat_map(|v| v.to_le_bytes()).collect()
}

fn one_page_pdf() -> tempfile::NamedTempFile {
    let file = tempfile::Builder::new().suffix(".pdf").tempfile().unwrap();
    let surface = cairo::PdfSurface::new(300.0, 200.0, file.path()).unwrap();
    let cr = cairo::Context::new(&surface).unwrap();
    cr.set_font_size(20.0);
    cr.move_to(20.0, 50.0);
    cr.show_text("Sign here").unwrap();
    drop(cr);
    surface.finish();
    file
}

/// The annotation types on page 1 of the saved file, read back through poppler.
fn saved_types(path: &std::path::Path) -> Vec<poppler::AnnotType> {
    let doc = poppler::Document::from_file(&glib::filename_to_uri(path, None).unwrap(), None).unwrap();
    doc.page(0)
        .unwrap()
        .annot_mapping()
        .iter()
        .map(|m| {
            use glib::translate::{FromGlib, ToGlibPtr};
            // poppler-rs has no accessor for PopplerAnnotMapping.annot; the mapping owns it while `m` lives.
            #[allow(unsafe_code)]
            unsafe {
                let raw: *const poppler::ffi::PopplerAnnotMapping = m.to_glib_none().0;
                poppler::AnnotType::from_glib(poppler::ffi::poppler_annot_get_annot_type((*raw).annot))
            }
        })
        .collect()
}

#[test]
fn annotations_survive_a_save_and_a_removed_one_does_not() {
    let file = one_page_pdf();
    let helper = Helper::start(file.path());

    let mut highlight = 1u32.to_le_bytes().to_vec();
    highlight.extend(f32s(&[20.0, 30.0, 120.0, 55.0]));
    let added = helper.ask(Msg::new(OP_ANNOT_ADD, 0, ANNOT_HIGHLIGHT, 0xffd400, 0), Some(body(&highlight)));
    assert_eq!(added.op, OP_ANNOT_REPLY, "error code {}", added.a);

    let mut note = f32s(&[200.0, 40.0]);
    note.extend_from_slice("Check the date".as_bytes());
    let note_id = helper.ask(Msg::new(OP_ANNOT_ADD, 0, ANNOT_NOTE, 0xffd400, 0), Some(body(&note))).b;

    let mut ink = 1u32.to_le_bytes().to_vec();
    ink.extend(3u32.to_le_bytes());
    ink.extend(f32s(&[20.0, 150.0, 60.0, 120.0, 100.0, 160.0]));
    assert_eq!(helper.ask(Msg::new(OP_ANNOT_ADD, 0, ANNOT_INK, 0x0000ff, 0), Some(body(&ink))).op, OP_ANNOT_REPLY);

    let mut stamp = f32s(&[150.0, 120.0, 270.0, 180.0]);
    stamp.extend(2u32.to_le_bytes());
    stamp.extend(2u32.to_le_bytes());
    stamp.extend([0u8, 0, 0, 255].repeat(4));
    assert_eq!(helper.ask(Msg::new(OP_ANNOT_ADD, 0, ANNOT_STAMP, 0, 0), Some(body(&stamp))).op, OP_ANNOT_REPLY);

    assert_eq!(helper.ask(Msg::new(OP_ANNOT_REMOVE, 0, note_id, 0, 0), None).op, OP_ANNOT_REPLY);

    let out = tempfile::Builder::new().suffix(".pdf").tempfile().unwrap();
    let target = std::fs::OpenOptions::new().write(true).open(out.path()).unwrap();
    assert_eq!(helper.ask(Msg::new(OP_SAVE, 0, 0, 0, 0), Some(OwnedFd::from(target))).op, OP_SAVED);

    let saved = saved_types(out.path());
    for kind in [poppler::AnnotType::Highlight, poppler::AnnotType::Ink, poppler::AnnotType::Stamp] {
        assert!(saved.contains(&kind), "{kind:?} missing in: {saved:?}");
    }
    assert!(!saved.contains(&poppler::AnnotType::Text), "the removed note was saved: {saved:?}");
}

#[test]
fn malformed_body_is_refused_and_the_helper_keeps_serving() {
    let file = one_page_pdf();
    let helper = Helper::start(file.path());
    // Says 5 rects, carries one.
    let mut bad = 5u32.to_le_bytes().to_vec();
    bad.extend(f32s(&[0.0, 0.0, 1.0, 1.0]));
    let reply = helper.ask(Msg::new(OP_ANNOT_ADD, 0, ANNOT_HIGHLIGHT, 0, 0), Some(body(&bad)));
    assert_eq!(reply.op, proto::OP_ERROR);
    let mut good = 1u32.to_le_bytes().to_vec();
    good.extend(f32s(&[0.0, 0.0, 10.0, 10.0]));
    assert_eq!(helper.ask(Msg::new(OP_ANNOT_ADD, 0, ANNOT_HIGHLIGHT, 0, 0), Some(body(&good))).op, OP_ANNOT_REPLY);
}
