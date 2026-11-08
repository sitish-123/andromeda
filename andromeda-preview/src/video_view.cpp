#include "video_view.h"

#include "core/log.h"
#include "linux-dmabuf-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "wayland/wayland_connection.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <unistd.h>
#include <wayland-client.h>

namespace preview {

  namespace {
    constexpr Logger kLog("preview");
    constexpr std::uint32_t kXrgb8888 = 0x34325258; // DRM fourcc 'XR24'
    constexpr std::uint64_t kModLinear = 0;
    constexpr std::uint64_t kModInvalid = 0x00ffffffffffffffULL;
    constexpr std::uint32_t kMaxSide = 8192;
    constexpr std::uint32_t kMaxPlanes = 4;
    // Frames asked for or queued ahead of the shown one; the helper holds at most 4 dmabuf frames.
    constexpr std::size_t kAhead = 2;
    constexpr std::uint32_t kErrBusy = 6;
    constexpr long kDmaBufMagic = 0x444d4142; // statfs f_type of a dmabuf fd
    constexpr std::chrono::milliseconds kBusyRetry{10};

    // Planes per format and the row divisor of each plane (4:2:0 chroma has half the rows); 0 = unknown format.
    struct FormatShape {
      std::uint32_t planes = 0;
      std::uint32_t bytesPerPixel = 0; // per plane row: NV12 chroma rows hold width bytes of interleaved UV
    };
    FormatShape shapeOf(std::uint32_t fourcc) {
      switch (fourcc) {
      case 0x3231564e: // NV12
        return {2, 1};
      case 0x30313050: // P010
        return {2, 2};
      case 0x34325258: // XR24
      case 0x34325241: // AR24
      case 0x34324258: // XB24
      case 0x34324241: // AB24
        return {1, 4};
      default:
        return {};
      }
    }

    std::uint32_t u32At(const std::vector<std::uint8_t>& bytes, std::size_t at) {
      std::uint32_t v = 0;
      std::memcpy(&v, bytes.data() + at, sizeof(v));
      return v;
    }
    std::uint64_t u64At(const std::vector<std::uint8_t>& bytes, std::size_t at) {
      std::uint64_t v = 0;
      std::memcpy(&v, bytes.data() + at, sizeof(v));
      return v;
    }

    void onFormat(void*, zwp_linux_dmabuf_v1*, std::uint32_t) {}
    void onModifier(void* data, zwp_linux_dmabuf_v1*, std::uint32_t format, std::uint32_t hi, std::uint32_t lo) {
      static_cast<std::set<std::pair<std::uint32_t, std::uint64_t>>*>(data)->emplace(
          format, (static_cast<std::uint64_t>(hi) << 32) | lo);
    }
    constexpr zwp_linux_dmabuf_v1_listener kDmabufListener{.format = onFormat, .modifier = onModifier};

    void onGlobal(void* data, wl_registry* registry, std::uint32_t name, const char* interface, std::uint32_t version) {
      // Version 3: the compositor lists format and modifier pairs right after the bind.
      if (std::strcmp(interface, zwp_linux_dmabuf_v1_interface.name) == 0 && version >= 3) {
        *static_cast<zwp_linux_dmabuf_v1**>(data) =
            static_cast<zwp_linux_dmabuf_v1*>(wl_registry_bind(registry, name, &zwp_linux_dmabuf_v1_interface, 3));
      }
    }
    void onGlobalRemove(void*, wl_registry*, std::uint32_t) {}
    constexpr wl_registry_listener kRegistryListener{.global = onGlobal, .global_remove = onGlobalRemove};
  } // namespace

  VideoView::VideoView(WaylandConnection& wayland, HelperClient& helper, Callbacks callbacks)
      : m_wayland(wayland), m_helper(helper), m_callbacks(std::move(callbacks)) {}

  VideoView::~VideoView() {
    m_timer.stop();
    m_busyRetry.stop();
    cancelCheck();
    for (auto& frame : m_queue) {
      if (frame.buffer != nullptr) {
        wl_buffer_destroy(frame.buffer);
      }
    }
    for (auto& buffer : m_buffers) {
      wl_buffer_destroy(buffer->buffer);
    }
    if (m_firstFrameCallback != nullptr) {
      wl_callback_destroy(m_firstFrameCallback);
    }
    if (m_viewport != nullptr) {
      wp_viewport_destroy(m_viewport);
    }
    if (m_subsurface != nullptr) {
      wl_subsurface_destroy(m_subsurface);
    }
    if (m_surface != nullptr) {
      wl_surface_destroy(m_surface);
    }
    if (m_dmabuf != nullptr) {
      zwp_linux_dmabuf_v1_destroy(m_dmabuf);
    }
  }

  bool VideoView::attach(wl_surface* parent, bool above) {
    wl_display* display = m_wayland.display();
    if (m_wayland.subcompositor() == nullptr || m_wayland.viewporter() == nullptr) {
      kLog.warn("video needs wl_subcompositor and wp_viewporter");
      return false;
    }
    wl_registry* registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &kRegistryListener, &m_dmabuf);
    wl_display_roundtrip(display);
    wl_registry_destroy(registry);
    if (m_dmabuf != nullptr) {
      zwp_linux_dmabuf_v1_add_listener(m_dmabuf, &kDmabufListener, &m_formats);
      wl_display_roundtrip(display);
    }

    m_surface = wl_compositor_create_surface(m_wayland.compositor());
    m_subsurface = wl_subcompositor_get_subsurface(m_wayland.subcompositor(), m_surface, parent);
    m_viewport = wp_viewporter_get_viewport(m_wayland.viewporter(), m_surface);
    wl_subsurface_set_desync(m_subsurface);
    // Below: the window leaves a hole for the picture and draws its r12 corner masks over the edge.
    if (above) {
      wl_subsurface_place_above(m_subsurface, parent);
    } else {
      wl_subsurface_place_below(m_subsurface, parent);
    }
    // Clicks fall through to the window, which owns play and pause.
    wl_region* empty = wl_compositor_create_region(m_wayland.compositor());
    wl_surface_set_input_region(m_surface, empty);
    wl_region_destroy(empty);
    requestFrames();
    return true;
  }

  void VideoView::setVideoSize(std::uint32_t width, std::uint32_t height) {
    if (width == m_videoWidth && height == m_videoHeight) {
      return;
    }
    m_videoWidth = width;
    m_videoHeight = height;
    applyGeometry();
  }

  void VideoView::place(float x, float y, float width, float height) {
    if (x == m_boxX && y == m_boxY && width == m_boxW && height == m_boxH) {
      return;
    }
    m_boxX = x;
    m_boxY = y;
    m_boxW = width;
    m_boxH = height;
    applyGeometry();
    schedule();
  }

  void VideoView::applyGeometry() {
    if (m_surface == nullptr || m_videoWidth == 0 || m_videoHeight == 0 || m_boxW < 1.0F || m_boxH < 1.0F) {
      return;
    }
    const float scale = std::min(m_boxW / static_cast<float>(m_videoWidth), m_boxH / static_cast<float>(m_videoHeight));
    m_w = std::max(1.0F, std::floor(static_cast<float>(m_videoWidth) * scale));
    m_h = std::max(1.0F, std::floor(static_cast<float>(m_videoHeight) * scale));
    m_x = std::round(m_boxX + (m_boxW - m_w) / 2.0F);
    m_y = std::round(m_boxY + (m_boxH - m_h) / 2.0F);
    // The position applies with the parent's next commit, the size and opaque region with ours.
    wl_subsurface_set_position(m_subsurface, static_cast<std::int32_t>(m_x), static_cast<std::int32_t>(m_y));
    wp_viewport_set_destination(m_viewport, static_cast<std::int32_t>(m_w), static_cast<std::int32_t>(m_h));
    wl_region* opaque = wl_compositor_create_region(m_wayland.compositor());
    wl_region_add(opaque, 0, 0, static_cast<std::int32_t>(m_w), static_cast<std::int32_t>(m_h));
    wl_surface_set_opaque_region(m_surface, opaque);
    wl_region_destroy(opaque);
    wl_surface_commit(m_surface);
  }

  bool VideoView::contains(float x, float y) const {
    return m_w > 0.0F && x >= m_x && x < m_x + m_w && y >= m_y && y < m_y + m_h;
  }

  void VideoView::togglePlaying() {
    m_playing = !m_playing;
    // Resume rebases the clock on the next frame, so playback continues where it stopped.
    m_hasBase = false;
    if (m_playing) {
      schedule();
    } else {
      m_timer.stop();
    }
  }

  void VideoView::requestFrames() {
    while (!m_failed && !m_ended && !m_busy && m_inFlight + m_queue.size() < kAhead) {
      ++m_inFlight;
      m_helper.request({.op = op::Frame}, [this](const HelperReply& reply) { onReply(reply); });
    }
  }

  void VideoView::onReply(const HelperReply& reply) {
    --m_inFlight;
    if (m_failed) {
      return;
    }
    const auto& m = reply.msg;
    if (m.op == op::End) {
      m_ended = true;
      return;
    }
    if (m.op == op::Error && m.a == kErrBusy) {
      // A release may already be on its way to the helper: ask again shortly.
      m_busy = true;
      m_busyRetry.start(kBusyRetry, [this] {
        m_busy = false;
        requestFrames();
      });
      return;
    }
    // Untrusted: the payload must describe each plane, and each plane must name a passed fd.
    const bool valid = m.op == op::FrameReply && m.a > 0 && m.b > 0 && m.a <= kMaxSide && m.b <= kMaxSide && m.d > 0
        && m.d <= kMaxPlanes && reply.payload.size() == 16 + static_cast<std::size_t>(m.d) * 12 && !reply.fds.empty();
    if (!valid) {
      fail();
      return;
    }
    for (std::uint32_t plane = 0; plane < m.d; ++plane) {
      if (u32At(reply.payload, 16 + plane * 12) >= reply.fds.size()) {
        fail();
        return;
      }
    }
    m_queue.push_back({reply, std::chrono::nanoseconds(u64At(reply.payload, 8))});
    schedule();
    requestFrames();
  }

  void VideoView::schedule() {
    // No frame before the window has placed the video: an unsized surface would show at buffer size.
    if (m_queue.empty() || m_timer.active() || m_check != nullptr || m_w <= 0.0F || (m_shown && !m_playing)) {
      return;
    }
    const auto now = std::chrono::steady_clock::now();
    syncBase(now);
    if (!m_hasBase) {
      m_base = now - m_queue.front().pts;
      m_hasBase = true;
    }
    const auto due = m_base + m_queue.front().pts;
    if (due <= now) {
      presentDue();
      return;
    }
    m_timer.start(std::chrono::ceil<std::chrono::milliseconds>(due - now), [this] { presentDue(); });
  }

  void VideoView::syncBase(std::chrono::steady_clock::time_point now) {
    if (m_clock) {
      if (const auto position = m_clock()) {
        m_base = now - *position;
        m_hasBase = true;
      }
    }
  }

  void VideoView::presentDue() {
    // Inside its own callback the timer still counts as active: stop it so schedule() can start it again.
    m_timer.stop();
    if (m_queue.empty()) {
      return;
    }
    const auto now = std::chrono::steady_clock::now();
    syncBase(now);
    // Behind the clock: drop each frame whose successor is also due.
    while (m_queue.size() > 1 && m_base + m_queue[1].pts <= now) {
      if (m_queue.front().buffer != nullptr) {
        wl_buffer_destroy(m_queue.front().buffer);
      }
      release(m_queue.front().reply.msg.e);
      m_queue.pop_front();
    }
    if (m_queue.front().buffer == nullptr && fcntl(m_queue.front().reply.fds.front()->get(), F_GET_SEALS) < 0
        && layoutOf(m_queue.front().reply) != m_checkedLayout) {
      checkDmabuf();
      return;
    }
    Frame frame = std::move(m_queue.front());
    m_queue.pop_front();
    present(std::move(frame));
    if (m_failed) {
      return;
    }
    if (m_playing) {
      schedule();
    }
    requestFrames();
  }

  void VideoView::present(Frame frame) {
    wl_buffer* buffer = frame.buffer != nullptr ? frame.buffer : createBuffer(frame.reply);
    if (buffer == nullptr) {
      fail();
      return;
    }
    m_shownPts = frame.pts;
    auto entry = std::make_unique<Buffer>(Buffer{.view = this, .buffer = buffer, .id = frame.reply.msg.e});
    static constexpr wl_buffer_listener kListener{.release = &VideoView::onBufferRelease};
    wl_buffer_add_listener(buffer, &kListener, entry.get());
    m_buffers.push_back(std::move(entry));
    if (frame.reply.msg.a != m_videoWidth || frame.reply.msg.b != m_videoHeight) {
      setVideoSize(frame.reply.msg.a, frame.reply.msg.b);
      if (m_callbacks.moved) {
        m_callbacks.moved();
      }
    }
    wl_surface_attach(m_surface, buffer, 0, 0);
    wl_surface_damage_buffer(m_surface, 0, 0, INT32_MAX, INT32_MAX);
    if (!m_shown) {
      m_shown = true;
      m_firstFrameCallback = wl_surface_frame(m_surface);
      static constexpr wl_callback_listener kFrameListener{.done = &VideoView::onFrameDone};
      wl_callback_add_listener(m_firstFrameCallback, &kFrameListener, this);
    }
    wl_surface_commit(m_surface);
    wl_display_flush(m_wayland.display());
  }

  wl_buffer* VideoView::createBuffer(const HelperReply& reply) {
    const auto& m = reply.msg;
    const std::uint64_t modifier = u64At(reply.payload, 0);
    // A software frame is a sealed memfd (F_GET_SEALS fails on a dmabuf); it goes through wl_shm. Without the shrink
    // seal the helper could cut the file under the compositor's mapping.
    const int fd = reply.fds.front()->get();
    if (const int seals = fcntl(fd, F_GET_SEALS); seals >= 0) {
      if ((seals & F_SEAL_SHRINK) == 0) {
        return nullptr;
      }
      const std::uint32_t offset = u32At(reply.payload, 20);
      const std::uint32_t stride = u32At(reply.payload, 24);
      struct stat st{};
      const std::uint64_t end = static_cast<std::uint64_t>(offset) + static_cast<std::uint64_t>(stride) * m.b;
      if (m.c != kXrgb8888 || m.d != 1 || modifier != kModLinear || stride < m.a * 4 || fstat(fd, &st) != 0 || end > static_cast<std::uint64_t>(st.st_size)
          || st.st_size > INT32_MAX) {
        return nullptr;
      }
      wl_shm_pool* pool = wl_shm_create_pool(m_wayland.shm(), fd, static_cast<std::int32_t>(st.st_size));
      wl_buffer* buffer = wl_shm_pool_create_buffer(pool, static_cast<std::int32_t>(offset), static_cast<std::int32_t>(m.a),
                                                    static_cast<std::int32_t>(m.b), static_cast<std::int32_t>(stride),
                                                    WL_SHM_FORMAT_XRGB8888);
      wl_shm_pool_destroy(pool);
      return buffer;
    }
    zwp_linux_buffer_params_v1* params = dmabufParams(reply);
    if (params == nullptr) {
      return nullptr;
    }
    wl_buffer* buffer = zwp_linux_buffer_params_v1_create_immed(params, static_cast<std::int32_t>(m.a),
                                                                 static_cast<std::int32_t>(m.b), m.c, 0);
    zwp_linux_buffer_params_v1_destroy(params);
    return buffer;
  }

  zwp_linux_buffer_params_v1* VideoView::dmabufParams(const HelperReply& reply) {
    const auto& m = reply.msg;
    const std::uint64_t modifier = u64At(reply.payload, 0);
    // A create_immed the compositor cannot import ends the connection: check the advertised pairs first.
    if (m_dmabuf == nullptr || modifier == kModInvalid || !m_formats.contains({m.c, modifier})) {
      kLog.warn("compositor cannot import fourcc {:#x} modifier {:#x}", m.c, modifier);
      m_unsupported = true;
      return nullptr;
    }
    // Untrusted planes: each fd must be a dmabuf large enough for its plane, in the format's plane count.
    const FormatShape shape = shapeOf(m.c);
    if (shape.planes != m.d) {
      return nullptr;
    }
    for (std::uint32_t plane = 0; plane < m.d; ++plane) {
      const std::size_t at = 16 + static_cast<std::size_t>(plane) * 12;
      const int fd = reply.fds[u32At(reply.payload, at)]->get();
      struct statfs fs{};
      const off_t size = lseek(fd, 0, SEEK_END);
      const std::uint64_t rows = plane == 0 ? m.b : (m.b + 1) / 2;
      const std::uint64_t end = u32At(reply.payload, at + 4) + static_cast<std::uint64_t>(u32At(reply.payload, at + 8)) * rows;
      if (u32At(reply.payload, at + 8) < static_cast<std::uint64_t>(m.a) * shape.bytesPerPixel || fstatfs(fd, &fs) != 0 || fs.f_type != kDmaBufMagic || size < 0 || end > static_cast<std::uint64_t>(size)) {
        return nullptr;
      }
    }
    zwp_linux_buffer_params_v1* params = zwp_linux_dmabuf_v1_create_params(m_dmabuf);
    for (std::uint32_t plane = 0; plane < m.d; ++plane) {
      const std::size_t at = 16 + static_cast<std::size_t>(plane) * 12;
      zwp_linux_buffer_params_v1_add(params, reply.fds[u32At(reply.payload, at)]->get(), plane,
                                     u32At(reply.payload, at + 4), u32At(reply.payload, at + 8),
                                     static_cast<std::uint32_t>(modifier >> 32), static_cast<std::uint32_t>(modifier));
    }
    return params;
  }

  std::vector<std::uint64_t> VideoView::layoutOf(const HelperReply& reply) {
    std::vector<std::uint64_t> layout{reply.msg.a, reply.msg.b, reply.msg.c, u64At(reply.payload, 0)};
    for (std::uint32_t plane = 0; plane < reply.msg.d; ++plane) {
      const std::size_t at = 16 + static_cast<std::size_t>(plane) * 12;
      layout.push_back(u32At(reply.payload, at + 4));
      layout.push_back(u32At(reply.payload, at + 8));
    }
    return layout;
  }

  void VideoView::checkDmabuf() {
    const HelperReply& reply = m_queue.front().reply;
    zwp_linux_buffer_params_v1* params = dmabufParams(reply);
    if (params == nullptr) {
      fail();
      return;
    }
    m_check = new Check{.view = this, .params = params};
    static constexpr zwp_linux_buffer_params_v1_listener kListener{
        .created = &VideoView::onParamsCreated, .failed = &VideoView::onParamsFailed};
    zwp_linux_buffer_params_v1_add_listener(params, &kListener, m_check);
    zwp_linux_buffer_params_v1_create(params, static_cast<std::int32_t>(reply.msg.a),
                                      static_cast<std::int32_t>(reply.msg.b), reply.msg.c, 0);
    wl_display_flush(m_wayland.display());
  }

  void VideoView::cancelCheck() {
    // The Check outlives the view until the compositor answers, so a created buffer is not left behind.
    if (m_check != nullptr) {
      m_check->view = nullptr;
      m_check = nullptr;
    }
  }

  void VideoView::onParamsCreated(void* data, zwp_linux_buffer_params_v1* params, wl_buffer* buffer) {
    std::unique_ptr<Check> check(static_cast<Check*>(data));
    zwp_linux_buffer_params_v1_destroy(params);
    VideoView* self = check->view;
    if (self == nullptr) {
      wl_buffer_destroy(buffer);
      return;
    }
    self->m_check = nullptr;
    self->m_checkedLayout = layoutOf(self->m_queue.front().reply);
    self->m_queue.front().buffer = buffer;
    self->schedule();
  }

  void VideoView::onParamsFailed(void* data, zwp_linux_buffer_params_v1* params) {
    std::unique_ptr<Check> check(static_cast<Check*>(data));
    zwp_linux_buffer_params_v1_destroy(params);
    VideoView* self = check->view;
    if (self == nullptr) {
      return;
    }
    self->m_check = nullptr;
    kLog.warn("compositor rejected the dmabuf");
    self->m_unsupported = true;
    self->fail();
  }

  void VideoView::onBufferRelease(void* data, wl_buffer* buffer) {
    auto* entry = static_cast<Buffer*>(data);
    VideoView* self = entry->view;
    const std::uint32_t id = entry->id;
    wl_buffer_destroy(buffer);
    std::erase_if(self->m_buffers, [entry](const auto& b) { return b.get() == entry; });
    self->release(id);
    if (self->m_busy) {
      self->m_busy = false;
      self->requestFrames();
    }
  }

  void VideoView::onFrameDone(void* data, wl_callback* callback, std::uint32_t) {
    auto* self = static_cast<VideoView*>(data);
    wl_callback_destroy(callback);
    self->m_firstFrameCallback = nullptr;
    if (self->m_callbacks.firstFrame) {
      self->m_callbacks.firstFrame(self->m_videoWidth, self->m_videoHeight);
    }
  }

  void VideoView::release(std::uint32_t id) {
    if (!m_failed) {
      m_helper.notify({.op = op::Release, .a = id});
    }
  }

  void VideoView::fail() {
    if (m_failed) {
      return;
    }
    m_failed = true;
    m_timer.stop();
    m_busyRetry.stop();
    cancelCheck();
    for (auto& frame : m_queue) {
      if (frame.buffer != nullptr) {
        wl_buffer_destroy(frame.buffer);
      }
    }
    m_queue.clear();
    if (m_callbacks.failed) {
      m_callbacks.failed(m_unsupported);
    }
  }

} // namespace preview
