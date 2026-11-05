//! Video decoding for Quick Look (S7c): GStreamer with VA-API, frames out as dmabufs the window hands to the
//! compositor. Audio pads are dropped here; S7d adds them.

use crate::proto::{
    ERR_BUSY, ERR_DECODE, FLAG_AUDIO, FLAG_HW_DECODE, FLAG_VIDEO, KIND_MEDIA, Msg, OP_END, OP_FRAME_REPLY, OP_INFO_REPLY,
};
use gstreamer as gst;
use gstreamer::prelude::*;
use gstreamer_allocators::DmaBufMemory;
use gstreamer_app::AppSink;
use rustix::fd::{BorrowedFd, OwnedFd};
use std::collections::HashMap;
use std::fs::File;
use std::sync::{Arc, Mutex};

/// DRM fourcc 'XR24' (BGRx in memory), used for frames copied out of a software decoder.
const DRM_FORMAT_XRGB8888: u32 = u32::from_le_bytes(*b"XR24");
const DRM_FORMAT_MOD_LINEAR: u64 = 0;
const PREROLL: gst::ClockTime = gst::ClockTime::from_seconds(10);
/// Frames the window may hold at once; more would starve the decoder's buffer pool.
const MAX_HELD: usize = 4;

/// Loads the plugin registry. Call it before the sandbox: the registry cache lives under the user's cache.
///
/// # Errors
/// GStreamer cannot start.
pub fn init() -> Result<(), glib::Error> {
    gst::init()
}

pub struct Frame {
    pub msg: Msg,
    pub payload: Vec<u8>,
    pub fds: Vec<OwnedFd>,
}

pub struct Media {
    pipeline: gst::Pipeline,
    sink: AppSink,
    _file: File,
    flags: u32,
    width: u32,
    height: u32,
    playing: bool,
    next_id: u32,
    /// Frames the window still shows: their buffers must stay alive until it releases them.
    held: HashMap<u32, gst::Sample>,
}

impl Drop for Media {
    fn drop(&mut self) {
        let _ = self.pipeline.set_state(gst::State::Null);
    }
}

/// A seekable source over the already-open file: demuxers seek (an MP4 index often sits at the end), and the
/// sandbox has no path access to reopen it.
fn file_source(file: &File) -> Result<gstreamer_app::AppSrc, u32> {
    use std::os::unix::fs::FileExt;
    let size = file.metadata().map_err(|_| ERR_DECODE)?.len();
    let src = gstreamer_app::AppSrc::builder()
        .stream_type(gstreamer_app::AppStreamType::RandomAccess)
        .size(i64::try_from(size).map_err(|_| ERR_DECODE)?)
        .format(gst::Format::Bytes)
        .build();
    let reader = file.try_clone().map_err(|_| ERR_DECODE)?;
    let offset = Arc::new(Mutex::new(0u64));
    let seek_offset = Arc::clone(&offset);
    src.set_callbacks(
        gstreamer_app::AppSrcCallbacks::builder()
            .need_data(move |src, length| {
                let mut at = offset.lock().unwrap();
                let mut chunk = vec![0u8; usize::try_from(length).unwrap_or(4096).clamp(4096, 1 << 20)];
                match reader.read_at(&mut chunk, *at) {
                    Ok(0) | Err(_) => {
                        let _ = src.end_of_stream();
                    }
                    Ok(n) => {
                        chunk.truncate(n);
                        let mut buffer = gst::Buffer::from_mut_slice(chunk);
                        buffer.get_mut().unwrap().set_offset(*at);
                        *at += n as u64;
                        let _ = src.push_buffer(buffer);
                    }
                }
            })
            .seek_data(move |_, to| {
                *seek_offset.lock().unwrap() = to;
                true
            })
            .build(),
    );
    Ok(src)
}

/// Hardware decoders hand over dmabufs as they are; they require VideoMeta support downstream, which an appsink
/// only offers through its allocation callback. Software frames go through videoconvert into BGRx.
fn video_branch(hw: bool) -> Result<(gst::Element, AppSink), u32> {
    if hw {
        // vapostproc converts whatever the decoder makes (10-bit, odd tiling) into a dmabuf format it can export.
        let post = gst::ElementFactory::make("vapostproc").build().map_err(|_| ERR_DECODE)?;
        let bin = gst::Bin::new();
        let sink = AppSink::builder().sync(false).max_buffers(3).enable_last_sample(false).build();
        sink.set_caps(Some(&gst::Caps::builder("video/x-raw").features(["memory:DMABuf"]).field("format", "DMA_DRM").build()));
        sink.set_callbacks(
            gstreamer_app::AppSinkCallbacks::builder()
                .propose_allocation(|_, query| {
                    query.add_allocation_meta::<gstreamer_video::VideoMeta>(None);
                    true
                })
                .build(),
        );
        bin.add_many([&post, sink.upcast_ref()]).map_err(|_| ERR_DECODE)?;
        post.link(&sink).map_err(|_| ERR_DECODE)?;
        let ghost = gst::GhostPad::with_target(&post.static_pad("sink").ok_or(ERR_DECODE)?).map_err(|_| ERR_DECODE)?;
        bin.add_pad(&ghost).map_err(|_| ERR_DECODE)?;
        return Ok((bin.upcast(), sink));
    }
    let bin = gst::parse::bin_from_description("videoconvert ! video/x-raw,format=BGRx ! appsink name=sink", true)
        .map_err(|_| ERR_DECODE)?;
    let sink = bin.by_name("sink").and_then(|e| e.downcast::<AppSink>().ok()).ok_or(ERR_DECODE)?;
    sink.set_property("sync", false);
    sink.set_property("max-buffers", 3u32);
    sink.set_property("enable-last-sample", false);
    Ok((bin.upcast(), sink))
}

impl Media {
    /// Builds the pipeline and opens the VA display, before the sandbox: VA finds render nodes through udev and
    /// sysfs, which the sandbox hides. No file data is read here. The display reaches the decoder later as a
    /// pipeline context.
    /// `hw` false ranks the VA features out, so decodebin3 picks a software decoder and frames arrive as BGRx.
    #[must_use]
    pub fn prepare(hw: bool) -> gst::Pipeline {
        let pipeline = gst::Pipeline::new();
        if !hw {
            for feature in gst::Registry::get().features_by_plugin("va") {
                feature.set_rank(gst::Rank::NONE);
            }
            return pipeline;
        }
        if let Ok(probe) = gst::ElementFactory::make("vapostproc").build()
            && pipeline.add(&probe).is_ok()
        {
            let _ = probe.set_state(gst::State::Ready);
            let _ = probe.set_state(gst::State::Null);
            let _ = pipeline.remove(&probe);
        }
        pipeline
    }

    /// Prerolls the file and reports its size, duration and streams. Runs inside the sandbox.
    ///
    /// # Errors
    /// `ERR_DECODE` when the file has no decodable video stream.
    pub fn open(pipeline: gst::Pipeline, file: File) -> Result<Self, u32> {
        let src = file_source(&file)?;
        let decode = gst::ElementFactory::make("decodebin3").build().map_err(|_| ERR_DECODE)?;
        pipeline.add_many([src.upcast_ref::<gst::Element>(), &decode]).map_err(|_| ERR_DECODE)?;
        src.link(&decode).map_err(|_| ERR_DECODE)?;

        let found: Arc<Mutex<(Option<AppSink>, u32)>> = Arc::default();
        let pipeline_weak = pipeline.downgrade();
        let found_in = Arc::clone(&found);
        decode.connect_pad_added(move |_, pad| {
            let Some(pipeline) = pipeline_weak.upgrade() else { return };
            let caps = pad.current_caps().unwrap_or_else(|| pad.query_caps(None));
            let Some(s) = caps.structure(0) else { return };
            let mut state = found_in.lock().unwrap();
            if s.name().starts_with("audio/") {
                state.1 |= FLAG_AUDIO;
            }
            if !s.name().starts_with("video/") || state.0.is_some() {
                let sink = gst::ElementFactory::make("fakesink").property("sync", false).build().unwrap();
                let _ = pipeline.add(&sink);
                let _ = pad.link(&sink.static_pad("sink").unwrap());
                let _ = sink.sync_state_with_parent();
                return;
            }
            // The pad's caps are not negotiated yet: ask whether a VA decoder sits behind it.
            let va = gst::Caps::builder("video/x-raw").features(["memory:VAMemory"]).build();
            let hw = pad.query_caps(None).can_intersect(&va);
            let Ok((branch, sink)) = video_branch(hw).or_else(|_| video_branch(false)) else { return };
            let _ = pipeline.add(&branch);
            let Some(sink_pad) = branch.static_pad("sink") else { return };
            if pad.link(&sink_pad).is_err() {
                let _ = pipeline.remove(&branch);
                return;
            }
            let _ = branch.sync_state_with_parent();
            state.0 = Some(sink);
            state.1 |= FLAG_VIDEO | if hw { FLAG_HW_DECODE } else { 0 };
        });

        pipeline.set_state(gst::State::Paused).map_err(|_| ERR_DECODE)?;
        // decodebin3 adds its pads after typefinding; PAUSED can complete before any sink exists, so wait for the
        // video branch (or an error), then for its preroll.
        let fail = |pipeline: &gst::Pipeline| {
            let _ = pipeline.set_state(gst::State::Null);
            ERR_DECODE
        };
        let bus = pipeline.bus().ok_or(ERR_DECODE)?;
        let types = [gst::MessageType::Error, gst::MessageType::Eos, gst::MessageType::AsyncDone];
        // AsyncDone: every sink, the video one included if the file has video, holds a buffer.
        if bus.timed_pop_filtered(PREROLL, &types).map(|m| m.type_()) != Some(gst::MessageType::AsyncDone) {
            return Err(fail(&pipeline));
        }
        if found.lock().unwrap().0.is_none() {
            return Err(fail(&pipeline));
        }
        let (sink, flags) = {
            let state = found.lock().unwrap();
            (state.0.clone(), state.1)
        };
        let sink = sink.ok_or(ERR_DECODE)?;
        let caps = sink.static_pad("sink").and_then(|p| p.current_caps()).ok_or(ERR_DECODE)?;
        let s = caps.structure(0).ok_or(ERR_DECODE)?;
        let width = s.get::<i32>("width").ok().and_then(|w| u32::try_from(w).ok()).ok_or(ERR_DECODE)?;
        let height = s.get::<i32>("height").ok().and_then(|h| u32::try_from(h).ok()).ok_or(ERR_DECODE)?;
        Ok(Self { pipeline, sink, _file: file, flags, width, height, playing: false, next_id: 1, held: HashMap::new() })
    }

    #[must_use]
    pub fn info(&self) -> Msg {
        let ms = self
            .pipeline
            .query_duration::<gst::ClockTime>()
            .map_or(0, |d| u32::try_from(d.mseconds()).unwrap_or(u32::MAX));
        let mut msg = Msg::new(OP_INFO_REPLY, KIND_MEDIA, self.width, self.height, ms);
        msg.e = self.flags;
        msg
    }

    /// The next decoded frame, or `OP_END` at the end of the stream.
    ///
    /// # Errors
    /// `ERR_DECODE` when a frame cannot be exported.
    pub fn next_frame(&mut self) -> Result<Frame, u32> {
        if !self.playing {
            self.pipeline.set_state(gst::State::Playing).map_err(|_| ERR_DECODE)?;
            self.playing = true;
        }
        if self.held.len() >= MAX_HELD {
            return Err(ERR_BUSY);
        }
        let Some(sample) = self.sink.try_pull_sample(gst::ClockTime::from_seconds(5)) else {
            if self.sink.is_eos() {
                return Ok(Frame { msg: Msg::new(OP_END, 0, 0, 0, 0), payload: Vec::new(), fds: Vec::new() });
            }
            return Err(ERR_DECODE);
        };
        let (frame, zero_copy) = export(&sample, self.next_id)?;
        // A software frame is already copied out; only dmabufs must outlive the send.
        if zero_copy {
            self.held.insert(self.next_id, sample);
        }
        self.next_id = self.next_id.wrapping_add(1).max(1);
        Ok(frame)
    }

    pub fn release(&mut self, id: u32) {
        self.held.remove(&id);
    }
}

#[allow(unsafe_code)]
fn dup(fd: i32) -> Result<OwnedFd, u32> {
    if fd < 0 {
        return Err(ERR_DECODE);
    }
    // SAFETY: the dmabuf memory of a sample we hold owns `fd` for the length of this call.
    let borrowed = unsafe { BorrowedFd::borrow_raw(fd) };
    rustix::io::fcntl_dupfd_cloexec(borrowed, 0).map_err(|_| ERR_DECODE)
}

fn export(sample: &gst::Sample, id: u32) -> Result<(Frame, bool), u32> {
    let buffer = sample.buffer().ok_or(ERR_DECODE)?;
    let caps = sample.caps().ok_or(ERR_DECODE)?;
    let pts = buffer.pts().map_or(0, gst::ClockTime::nseconds);
    let meta = buffer.meta::<gstreamer_video::VideoMeta>();
    let first = buffer.peek_memory(0);
    if let Some(dmabuf) = first.downcast_memory_ref::<DmaBufMemory>() {
        let info = gstreamer_video::VideoInfoDmaDrm::from_caps(caps).map_err(|_| ERR_DECODE)?;
        let meta = meta.ok_or(ERR_DECODE)?;
        let planes = meta.n_planes() as usize;
        let mut payload = Vec::with_capacity(16 + planes * 12);
        payload.extend_from_slice(&info.modifier().to_le_bytes());
        payload.extend_from_slice(&pts.to_le_bytes());
        let mut fds = Vec::new();
        let mut seen: Vec<i32> = Vec::new();
        for plane in 0..planes {
            let offset = meta.offset()[plane];
            // A plane lives in the memory block that covers its offset.
            let (range, skip) = buffer.find_memory(offset..offset + 1).ok_or(ERR_DECODE)?;
            let mem_idx = range.start;
            let mem = buffer.peek_memory(mem_idx);
            let fd = mem.downcast_memory_ref::<DmaBufMemory>().map_or(dmabuf.fd(), |m| m.fd());
            let index = seen.iter().position(|&f| f == fd).unwrap_or_else(|| {
                seen.push(fd);
                seen.len() - 1
            });
            payload.extend_from_slice(&u32::try_from(index).unwrap_or(0).to_le_bytes());
            let plane_offset = mem.offset() + skip;
            payload.extend_from_slice(&u32::try_from(plane_offset).map_err(|_| ERR_DECODE)?.to_le_bytes());
            payload.extend_from_slice(&u32::try_from(meta.stride()[plane]).map_err(|_| ERR_DECODE)?.to_le_bytes());
        }
        for fd in seen {
            fds.push(dup(fd)?);
        }
        let msg = Msg { op: OP_FRAME_REPLY, a: meta.width(), b: meta.height(), c: info.fourcc(), d: meta.n_planes(), e: id };
        return Ok((Frame { msg, payload, fds }, true));
    }
    // Software frame: copy into a sealed memfd.
    let video = gstreamer_video::VideoInfo::from_caps(caps).map_err(|_| ERR_DECODE)?;
    let map = buffer.map_readable().map_err(|_| ERR_DECODE)?;
    let fd = crate::decode::sealed("preview-frame", map.as_slice())?;
    let mut payload = Vec::with_capacity(28);
    payload.extend_from_slice(&DRM_FORMAT_MOD_LINEAR.to_le_bytes());
    payload.extend_from_slice(&pts.to_le_bytes());
    payload.extend_from_slice(&0u32.to_le_bytes());
    payload.extend_from_slice(&u32::try_from(video.offset()[0]).map_err(|_| ERR_DECODE)?.to_le_bytes());
    payload.extend_from_slice(&u32::try_from(video.stride()[0]).map_err(|_| ERR_DECODE)?.to_le_bytes());
    let msg = Msg { op: OP_FRAME_REPLY, a: video.width(), b: video.height(), c: DRM_FORMAT_XRGB8888, d: 1, e: id };
    Ok((Frame { msg, payload, fds: vec![fd] }, false))
}
