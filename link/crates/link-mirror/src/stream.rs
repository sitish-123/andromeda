//! The daemon's socket of access units into the decoding pipelines, in the stream's framing: a big-endian u32 length and
//! u64 presentation time in microseconds, a flags byte, then the bytes: H.264 in Annex B form, or with the audio flag
//! one Opus packet of 48 kHz stereo.

use std::io::{Read as _, Write as _};
use std::os::unix::net::UnixStream;
use std::path::{Path, PathBuf};
use std::time::{SystemTime, UNIX_EPOCH};

use anyhow::{Context, bail};
use gstreamer as gst;
use gstreamer::prelude::*;
use gstreamer_app as gst_app;

const UNIT_HEADER: usize = 13;
const MAX_UNIT: usize = 4 * 1024 * 1024;
const KEYFRAME: u8 = 1;
const AUDIO: u8 = 4;
const CAPS: &str = "video/x-h264,stream-format=byte-stream,alignment=au";
const AUDIO_CAPS: &str = "audio/x-opus,channel-mapping-family=(int)0,channels=(int)2,rate=(int)48000";
/// Every this many pixels of the luma plane go into a frame's mean; enough for a full-screen flip.
const LUMA_STRIDE: usize = 7;

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

/// Where the phone's sound goes.
pub enum Sound {
    Speakers,
    Wav(PathBuf),
    Mute,
}

/// The sound's pipeline, built at its first packet: phones without playback capture send none.
struct Audio {
    sound: Sound,
    running: Option<(gst::Pipeline, gst_app::AppSrc)>,
}

impl Audio {
    fn push(&mut self, buffer: gst::Buffer) {
        if self.running.is_none() {
            let sink = match &self.sound {
                Sound::Speakers => "autoaudiosink sync=false".to_owned(),
                Sound::Wav(path) => format!("wavenc ! filesink location={}", path.display()),
                Sound::Mute => return,
            };
            let description = format!(
                "appsrc name=src is-live=true format=time caps={AUDIO_CAPS} ! opusdec ! audioconvert ! audioresample \
                 ! {sink}"
            );
            match build(&description).and_then(|(pipeline, src)| {
                pipeline.set_state(gst::State::Playing)?;
                Ok((pipeline, src))
            }) {
                Ok(running) => self.running = Some(running),
                Err(error) => {
                    log::info!("no sound: {error}");
                    self.sound = Sound::Mute;
                    return;
                }
            }
        }
        if let Some((_, src)) = &self.running
            && src.push_buffer(buffer).is_err()
        {
            log::debug!("the sound pipeline stopped");
        }
    }

    /// Ends the sound, waiting for a file to be complete.
    fn finish(self) {
        let Some((pipeline, src)) = self.running else { return };
        if src.end_of_stream().is_ok()
            && let Some(bus) = pipeline.bus()
        {
            bus.timed_pop_filtered(gst::ClockTime::from_seconds(5), &[gst::MessageType::Eos, gst::MessageType::Error]);
        }
        if let Err(error) = pipeline.set_state(gst::State::Null) {
            log::debug!("stopping the sound: {error}");
        }
    }
}

/// Pushes units into `src` and the sound on a thread until the socket ends or `limit` video units went in, then ends
/// the streams.
fn feed(mut socket: UnixStream, src: gst_app::AppSrc, sound: Sound, limit: Option<u32>) {
    std::thread::spawn(move || {
        let mut audio = Audio { sound, running: None };
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
                if unit.flags & (KEYFRAME | AUDIO) == 0 {
                    buffer.set_flags(gst::BufferFlags::DELTA_UNIT);
                }
            }
            if unit.flags & AUDIO != 0 {
                audio.push(buffer);
                continue;
            }
            if src.push_buffer(buffer).is_err() {
                break;
            }
            fed += 1;
        }
        audio.finish();
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

/// Runs `pipeline` fed from `socket` until the stream ends; `count` video units at most.
fn run_to_end(
    pipeline: &gst::Pipeline,
    src: gst_app::AppSrc,
    socket: UnixStream,
    sound: Sound,
    count: u32,
) -> anyhow::Result<()> {
    pipeline.set_state(gst::State::Playing)?;
    feed(socket, src, sound, Some(count));
    let bus = pipeline.bus().context("the pipeline has no bus")?;
    let message =
        bus.timed_pop_filtered(gst::ClockTime::from_seconds(120), &[gst::MessageType::Eos, gst::MessageType::Error]);
    pipeline.set_state(gst::State::Null)?;
    match message.as_deref().map(gst::MessageRef::view) {
        Some(gst::MessageView::Eos(_)) => Ok(()),
        Some(gst::MessageView::Error(error)) => bail!("decoding: {}", error.error()),
        _ => bail!("decoding did not finish in 120 s"),
    }
}

/// Decodes `count` units into `dir/frame-NNN.png`, and the sound into `dir/audio.wav`, for tests; software decoding,
/// so it runs anywhere.
pub fn to_png(socket: UnixStream, dir: &Path, count: u32) -> anyhow::Result<()> {
    std::fs::create_dir_all(dir)?;
    let (pipeline, src) = build(&format!(
        "appsrc name=src is-live=true format=time caps={CAPS} ! h264parse ! avdec_h264 ! videoconvert ! pngenc \
         ! multifilesink sync=false location={}/frame-%03d.png",
        dir.display()
    ))?;
    run_to_end(&pipeline, src, socket, Sound::Wav(dir.join("audio.wav")), count)
}

/// Decodes `count` units and writes one line per frame to `out`: the Unix time in ms it was decoded at, and its mean
/// luma (0 to 255). A test flips the phone's screen and times the flip's arrival.
pub fn to_luma(socket: UnixStream, out: &Path, count: u32) -> anyhow::Result<()> {
    let (pipeline, src) = build(&format!(
        "appsrc name=src is-live=true format=time caps={CAPS} ! h264parse ! avdec_h264 ! videoconvert \
         ! video/x-raw,format=GRAY8 ! appsink name=sink sync=false"
    ))?;
    let sink = pipeline
        .by_name("sink")
        .and_then(|sink| sink.downcast::<gst_app::AppSink>().ok())
        .context("the pipeline has no appsink")?;
    let mut file = std::io::BufWriter::new(std::fs::File::create(out)?);
    sink.set_callbacks(
        gst_app::AppSinkCallbacks::builder()
            .new_sample(move |sink| {
                let sample = sink.pull_sample().map_err(|_| gst::FlowError::Eos)?;
                let buffer = sample.buffer().ok_or(gst::FlowError::Error)?;
                let map = buffer.map_readable().map_err(|_| gst::FlowError::Error)?;
                let picked = map.iter().step_by(LUMA_STRIDE);
                let (sum, n) = picked.fold((0u64, 0u64), |(sum, n), luma| (sum + u64::from(*luma), n + 1));
                let now = SystemTime::now().duration_since(UNIX_EPOCH).map_or(0, |since| since.as_millis());
                writeln!(file, "{now} {}", sum / n.max(1))
                    .and_then(|()| file.flush())
                    .map_err(|_| gst::FlowError::Error)?;
                Ok(gst::FlowSuccess::Ok)
            })
            .build(),
    );
    run_to_end(&pipeline, src, socket, Sound::Mute, count)
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
        feed(socket.try_clone()?, src, Sound::Speakers, None);
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
