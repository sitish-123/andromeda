//! The daemon's socket of access units into the decoding pipeline, in the stream's framing: a big-endian u32 length and u64
//! presentation time in microseconds, a flags byte, then the Annex B bytes.

use std::io::Read as _;
use std::os::unix::net::UnixStream;
use std::path::Path;

use anyhow::{Context, bail};
use gstreamer as gst;
use gstreamer::prelude::*;
use gstreamer_app as gst_app;

const UNIT_HEADER: usize = 13;
const MAX_UNIT: usize = 4 * 1024 * 1024;
const KEYFRAME: u8 = 1;
const CAPS: &str = "video/x-h264,stream-format=byte-stream,alignment=au";

struct Unit {
    pts_us: u64,
    flags: u8,
    data: Vec<u8>,
}

fn read_unit(socket: &mut UnixStream) -> std::io::Result<Option<Unit>> {
    let mut header = [0; UNIT_HEADER];
    match socket.read_exact(&mut header) {
        Ok(()) => {}
        Err(error) if error.kind() == std::io::ErrorKind::UnexpectedEof => return Ok(None),
        Err(error) => return Err(error),
    }
    let [a, b, c, d, pts @ .., flags] = header;
    let len = u32::from_be_bytes([a, b, c, d]) as usize;
    if len > MAX_UNIT {
        return Err(std::io::Error::new(std::io::ErrorKind::InvalidData, "an access unit over 4 MiB"));
    }
    let mut data = vec![0; len];
    socket.read_exact(&mut data)?;
    Ok(Some(Unit { pts_us: u64::from_be_bytes(pts), flags, data }))
}

/// Pushes units into `src` on a thread until the socket ends or `limit` units went in, then ends the stream.
fn feed(mut socket: UnixStream, src: gst_app::AppSrc, limit: Option<u32>) {
    std::thread::spawn(move || {
        let mut first = None;
        let mut fed = 0;
        while limit.is_none_or(|limit| fed < limit) {
            let unit = match read_unit(&mut socket) {
                Ok(Some(unit)) => unit,
                Ok(None) => break,
                Err(error) => {
                    log::info!("reading the mirror stream: {error}");
                    break;
                }
            };
            let base = *first.get_or_insert(unit.pts_us);
            let mut buffer = gst::Buffer::from_mut_slice(unit.data);
            if let Some(buffer) = buffer.get_mut() {
                buffer.set_pts(gst::ClockTime::from_useconds(unit.pts_us.saturating_sub(base)));
                if unit.flags & KEYFRAME == 0 {
                    buffer.set_flags(gst::BufferFlags::DELTA_UNIT);
                }
            }
            if src.push_buffer(buffer).is_err() {
                break;
            }
            fed += 1;
        }
        if let Err(error) = src.end_of_stream() {
            log::debug!("ending the mirror stream: {error}");
        }
    });
}

fn build(description: &str) -> anyhow::Result<(gst::Pipeline, gst_app::AppSrc)> {
    let pipeline =
        gst::parse::launch(description)?.downcast::<gst::Pipeline>().map_err(|_| anyhow::anyhow!("not a pipeline"))?;
    let src = pipeline
        .by_name("src")
        .and_then(|src| src.downcast::<gst_app::AppSrc>().ok())
        .context("the pipeline has no appsrc")?;
    Ok((pipeline, src))
}

/// Decodes `count` units into `dir/frame-NNN.png`, for tests; software decoding, so it runs anywhere.
pub fn to_png(socket: UnixStream, dir: &Path, count: u32) -> anyhow::Result<()> {
    std::fs::create_dir_all(dir)?;
    let (pipeline, src) = build(&format!(
        "appsrc name=src is-live=true format=time caps={CAPS} ! h264parse ! avdec_h264 ! videoconvert ! pngenc \
         ! multifilesink sync=false location={}/frame-%03d.png",
        dir.display()
    ))?;
    pipeline.set_state(gst::State::Playing)?;
    feed(socket, src, Some(count));
    let bus = pipeline.bus().context("the pipeline has no bus")?;
    let message =
        bus.timed_pop_filtered(gst::ClockTime::from_seconds(60), &[gst::MessageType::Eos, gst::MessageType::Error]);
    pipeline.set_state(gst::State::Null)?;
    match message.as_deref().map(gst::MessageRef::view) {
        Some(gst::MessageView::Eos(_)) => Ok(()),
        Some(gst::MessageView::Error(error)) => bail!("decoding: {}", error.error()),
        _ => bail!("decoding did not finish in 60 s"),
    }
}

/// The window's pipeline: VA-API decoding when the GPU has it, into a GTK paintable, never waiting on the clock.
pub struct Display {
    pub pipeline: gst::Pipeline,
    pub paintable: gtk4::gdk::Paintable,
    socket: UnixStream,
}

impl Display {
    pub fn new(socket: UnixStream) -> anyhow::Result<Self> {
        if gst::ElementFactory::find("gtk4paintablesink").is_none() {
            bail!("gtk4paintablesink is missing; install gst-plugin-gtk4");
        }
        let decoder =
            if gst::ElementFactory::find("vah264dec").is_some() { "vah264dec" } else { "avdec_h264 ! videoconvert" };
        log::info!("decoding with {}", decoder.split(' ').next().unwrap_or(decoder));
        let (pipeline, src) = build(&format!(
            "appsrc name=src is-live=true format=time caps={CAPS} ! h264parse ! {decoder} \
             ! gtk4paintablesink name=sink sync=false"
        ))?;
        let sink = pipeline.by_name("sink").context("the pipeline has no sink")?;
        let paintable = sink.property::<gtk4::gdk::Paintable>("paintable");
        pipeline.set_state(gst::State::Playing)?;
        feed(socket.try_clone()?, src, None);
        Ok(Self { pipeline, paintable, socket })
    }

    /// Closes the socket, which tells the daemon and then the phone to stop.
    pub fn stop(&self) {
        if let Err(error) = self.socket.shutdown(std::net::Shutdown::Both) {
            log::debug!("closing the mirror socket: {error}");
        }
        if let Err(error) = self.pipeline.set_state(gst::State::Null) {
            log::debug!("stopping the pipeline: {error}");
        }
    }
}
