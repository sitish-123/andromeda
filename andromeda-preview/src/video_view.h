#pragma once

#include "core/timer_manager.h"
#include "helper_client.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <memory>
#include <set>
#include <utility>
#include <vector>

class WaylandConnection;
struct wl_buffer;
struct wl_callback;
struct wl_subsurface;
struct wl_surface;
struct wp_viewport;
struct zwp_linux_buffer_params_v1;
struct zwp_linux_dmabuf_v1;

namespace preview {

  // Plays the frames of a media helper on a desync subsurface: dmabufs go to the compositor unchanged
  // (scanout candidate), software frames go through wl_shm. The main surface does not redraw per frame.
  class VideoView {
  public:
    struct Callbacks {
      std::function<void(std::uint32_t width, std::uint32_t height)> firstFrame;
      // `unsupported`: the compositor cannot import the helper's dmabuf format; software frames would work.
      std::function<void(bool unsupported)> failed;
      // The video moved inside the window: the parent must commit once for the new subsurface position.
      std::function<void()> moved;
    };

    VideoView(WaylandConnection& wayland, HelperClient& helper, Callbacks callbacks);
    ~VideoView();
    VideoView(const VideoView&) = delete;
    VideoView& operator=(const VideoView&) = delete;

    // Creates the subsurface on `parent`. False when the compositor lacks a needed global.
    // `above`: over the window (a window that draws an opaque stage under the picture, as the Files Gallery does).
    bool attach(wl_surface* parent, bool above = false);
    // The box (parent-local, logical) the video fits into, keeping its aspect.
    void place(float x, float y, float width, float height);
    void setVideoSize(std::uint32_t width, std::uint32_t height);
    void togglePlaying();
    // Paces frames on this clock (stream time) while it has a value, else on steady_clock.
    void setClock(std::function<std::optional<std::chrono::nanoseconds>()> clock) { m_clock = std::move(clock); }
    [[nodiscard]] bool contains(float x, float y) const;
    // The picture inside the placed box, window-local logical px (x, y, width, height); zero size before a frame size.
    [[nodiscard]] std::array<float, 4> pictureRect() const noexcept { return {m_x, m_y, m_w, m_h}; }
    // The helper reported the end and the last frame is on screen.
    [[nodiscard]] bool finished() const noexcept { return m_ended && m_queue.empty() && m_inFlight == 0; }
    [[nodiscard]] bool playing() const noexcept { return m_playing; }
    // Stream time of the frame on screen.
    [[nodiscard]] std::chrono::nanoseconds shownPts() const noexcept { return m_shownPts; }

  private:
    struct Frame {
      HelperReply reply;
      std::chrono::nanoseconds pts{0};
      // Set once the compositor has accepted the first dmabuf (see checkDmabuf).
      wl_buffer* buffer = nullptr;
    };
    struct Buffer {
      VideoView* view = nullptr;
      wl_buffer* buffer = nullptr;
      std::uint32_t id = 0;
    };

    void requestFrames();
    void onReply(const HelperReply& reply);
    void schedule();
    void presentDue();
    void present(Frame frame);
    [[nodiscard]] wl_buffer* createBuffer(const HelperReply& reply);
    [[nodiscard]] zwp_linux_buffer_params_v1* dmabufParams(const HelperReply& reply);
    void checkDmabuf();
    void applyGeometry();
    void release(std::uint32_t id);
    void fail();

    static void onBufferRelease(void* data, wl_buffer* buffer);
    static void onFrameDone(void* data, wl_callback* callback, std::uint32_t time);
    static void onParamsCreated(void* data, zwp_linux_buffer_params_v1* params, wl_buffer* buffer);
    static void onParamsFailed(void* data, zwp_linux_buffer_params_v1* params);

    WaylandConnection& m_wayland;
    HelperClient& m_helper;
    Callbacks m_callbacks;
    zwp_linux_dmabuf_v1* m_dmabuf = nullptr;
    std::set<std::pair<std::uint32_t, std::uint64_t>> m_formats;
    wl_surface* m_surface = nullptr;
    wl_subsurface* m_subsurface = nullptr;
    wp_viewport* m_viewport = nullptr;
    wl_callback* m_firstFrameCallback = nullptr;
    // Buffers the compositor has not released yet.
    std::vector<std::unique_ptr<Buffer>> m_buffers;

    std::deque<Frame> m_queue;
    std::chrono::nanoseconds m_shownPts{0};
    std::uint32_t m_inFlight = 0;
    bool m_busy = false;
    bool m_ended = false;
    bool m_failed = false;
    bool m_unsupported = false;
    // A dmabuf layout the compositor has not imported yet goes through the async create, so a rejection leads to
    // media-sw, not a protocol error; frames in a checked layout use create_immed.
    struct Check {
      VideoView* view = nullptr; // null once cancelled: the reply then only frees the compositor's buffer
      zwp_linux_buffer_params_v1* params = nullptr;
    };
    std::vector<std::uint64_t> m_checkedLayout;
    Check* m_check = nullptr;
    void cancelCheck();
    [[nodiscard]] static std::vector<std::uint64_t> layoutOf(const HelperReply& reply);
    Timer m_busyRetry;
    bool m_playing = true;
    bool m_shown = false;
    std::chrono::steady_clock::time_point m_base{};
    bool m_hasBase = false;
    std::function<std::optional<std::chrono::nanoseconds>()> m_clock;
    void syncBase(std::chrono::steady_clock::time_point now);
    Timer m_timer;

    std::uint32_t m_videoWidth = 0;
    std::uint32_t m_videoHeight = 0;
    float m_boxX = 0, m_boxY = 0, m_boxW = 0, m_boxH = 0;
    float m_x = 0, m_y = 0, m_w = 0, m_h = 0;
  };

} // namespace preview
