//! Video frames from the sandboxed helper. Needs ffmpeg (libx264) to make the clip; the hardware test also needs
//! a VA-API H.264 decoder. Each test skips with a note where the machine lacks them.

use andromeda_preview_helper::proto::{
    self, FLAG_HW_DECODE, FLAG_VIDEO, KIND_MEDIA, Msg, OP_END, OP_FRAME, OP_FRAME_REPLY, OP_INFO, OP_RELEASE,
};
use command_fds::{CommandFdExt, FdMapping};
use rustix::fd::{AsFd, OwnedFd};
use rustix::net::{AddressFamily, RecvAncillaryBuffer, RecvAncillaryMessage, RecvFlags, SocketFlags, SocketType};
use std::io::IoSliceMut;
use std::mem::MaybeUninit;
use std::process::{Child, Command};

fn clip(dir: &std::path::Path) -> Option<std::path::PathBuf> {
    let path = dir.join("clip.mp4");
    let ok = Command::new("ffmpeg")
        .args(["-loglevel", "error", "-f", "lavfi", "-i", "color=c=red:size=640x360:rate=30", "-t", "1"])
        .args(["-c:v", "libx264", "-pix_fmt", "yuv420p"])
        .arg(&path)
        .status()
        .is_ok_and(|s| s.success());
    ok.then_some(path)
}

struct Helper {
    sock: OwnedFd,
    child: Child,
}

impl Helper {
    fn start(path: &std::path::Path, env: &[(&str, &str)]) -> Self {
        let (ours, theirs) =
            rustix::net::socketpair(AddressFamily::UNIX, SocketType::SEQPACKET, SocketFlags::CLOEXEC, None).unwrap();
        let mut cmd = Command::new(env!("CARGO_BIN_EXE_andromeda-preview-helper"));
        cmd.arg("media").arg(path).env_clear().env("HOME", std::env::var("HOME").unwrap());
        for (k, v) in env {
            cmd.env(k, v);
        }
        cmd.fd_mappings(vec![FdMapping { parent_fd: theirs, child_fd: 3 }]).unwrap();
        Self { sock: ours, child: cmd.spawn().unwrap() }
    }

    /// One reply with its payload and fds.
    fn ask(&self, msg: Msg) -> (Msg, Vec<u8>, Vec<OwnedFd>) {
        proto::send(self.sock.as_fd(), &msg, None).unwrap();
        let mut bytes = [0u8; proto::SIZE + proto::MAX_PAYLOAD];
        let mut space = [MaybeUninit::uninit(); rustix::cmsg_space!(ScmRights(proto::MAX_FDS))];
        let mut control = RecvAncillaryBuffer::new(&mut space);
        let got = rustix::net::recvmsg(&self.sock, &mut [IoSliceMut::new(&mut bytes)], &mut control, RecvFlags::CMSG_CLOEXEC)
            .unwrap();
        let mut fds = Vec::new();
        for m in control.drain() {
            if let RecvAncillaryMessage::ScmRights(r) = m {
                fds.extend(r);
            }
        }
        assert!(got.bytes >= proto::SIZE, "helper closed the socket");
        let header: &[u8; proto::SIZE] = bytes[..proto::SIZE].try_into().unwrap();
        (Msg::decode(header), bytes[proto::SIZE..got.bytes].to_vec(), fds)
    }
}

impl Drop for Helper {
    fn drop(&mut self) {
        let _ = self.child.kill();
        let _ = self.child.wait();
    }
}

/// Plays to the end: every frame has the clip's size, its planes point at fds that came with it, and the
/// stream ends with OP_END.
fn play(helper: &Helper) -> (u32, u32) {
    let mut frames = 0;
    let mut fourcc = 0;
    loop {
        let (msg, payload, fds) = helper.ask(Msg::new(OP_FRAME, 0, 0, 0, 0));
        if msg.op == OP_END {
            return (frames, fourcc);
        }
        assert_eq!(msg.op, OP_FRAME_REPLY);
        assert_eq!((msg.a, msg.b), (640, 360));
        assert_eq!(payload.len(), 16 + msg.d as usize * 12);
        for plane in 0..msg.d as usize {
            let word = |i: usize| u32::from_le_bytes(payload[16 + plane * 12 + i * 4..][..4].try_into().unwrap());
            let (index, offset, stride) = (word(0) as usize, u64::from(word(1)), u64::from(word(2)));
            assert!(index < fds.len());
            // Chroma planes of NV12 are half height; luma and BGRx need the full height.
            let rows = if plane == 0 { u64::from(msg.b) } else { u64::from(msg.b) / 2 };
            assert!(stride >= u64::from(msg.a), "stride {stride} under width");
            let size = rustix::fs::seek(&fds[index], rustix::fs::SeekFrom::End(0)).unwrap();
            assert!(size >= offset + stride * rows, "plane {plane} past its buffer");
        }
        if frames == 0 && msg.c == u32::from_le_bytes(*b"XR24") {
            // The clip is solid red: BGRx in memory.
            let file = std::fs::File::from(fds[0].try_clone().unwrap());
            let mut pixel = [0u8; 4];
            std::os::unix::fs::FileExt::read_exact_at(&file, &mut pixel, 0).unwrap();
            assert!(pixel[0] < 60 && pixel[1] < 60 && pixel[2] > 200, "first pixel {pixel:?} is not red");
        }
        fourcc = msg.c;
        frames += 1;
        proto::send(helper.sock.as_fd(), &Msg::new(OP_RELEASE, msg.e, 0, 0, 0), None).unwrap();
    }
}

#[test]
fn hardware_decode_gives_dmabuf_frames_to_the_end() {
    let dir = tempfile::tempdir().unwrap();
    let Some(path) = clip(dir.path()) else { return eprintln!("skipped: no ffmpeg/libx264") };
    let helper = Helper::start(&path, &[]);
    let (info, _, _) = helper.ask(Msg::new(OP_INFO, 0, 0, 0, 0));
    assert_eq!((info.a, info.b, info.c), (KIND_MEDIA, 640, 360));
    let va_here = std::path::Path::new("/dev/dri/renderD128").exists()
        && Command::new("gst-inspect-1.0").arg("vah264dec").output().is_ok_and(|o| o.status.success());
    if info.e & FLAG_HW_DECODE == 0 {
        assert!(!va_here, "a VA-API H.264 decoder exists, but the helper decoded in software");
        return eprintln!("skipped: no VA-API decoder here");
    }
    let (frames, fourcc) = play(&helper);
    assert_eq!(frames, 30);
    assert_eq!(fourcc, u32::from_le_bytes(*b"NV12"));
}

#[test]
fn software_decode_gives_bgrx_memfd_frames() {
    let dir = tempfile::tempdir().unwrap();
    let Some(path) = clip(dir.path()) else { return eprintln!("skipped: no ffmpeg/libx264") };
    let helper = Helper::start(&path, &[("GST_PLUGIN_FEATURE_RANK", "vah264dec:0")]);
    let (info, _, _) = helper.ask(Msg::new(OP_INFO, 0, 0, 0, 0));
    assert_eq!(info.e & (FLAG_VIDEO | FLAG_HW_DECODE), FLAG_VIDEO);
    let (frames, fourcc) = play(&helper);
    assert_eq!(frames, 30);
    assert_eq!(fourcc, u32::from_le_bytes(*b"XR24"));
}
