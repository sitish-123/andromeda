#pragma once

#include <EGL/egl.h>
#include <cstdint>
#include <memory>

class GlSharedContext;
class MpvPlayer;
class WaylandConnection;
struct mpv_render_context;
struct wl_callback;
struct wl_egl_window;
struct wl_subsurface;
struct wl_surface;

// The video: a desynchronized wl_subsurface below the window surface with its own EGL window surface and context,
// drawn by the libmpv render API. The compositor sees a plain client buffer of the video's size, so a fullscreen
// player with hidden controls can go to direct scanout or an overlay plane.
class VideoSurface {
public:
  VideoSurface(WaylandConnection& wayland, GlSharedContext& gl, MpvPlayer& player);
  ~VideoSurface();

  VideoSurface(const VideoSurface&) = delete;
  VideoSurface& operator=(const VideoSurface&) = delete;

  // Attaches below `parent`; false when EGL or the mpv render context fails.
  bool initialize(wl_surface* parent);
  // Rectangle in parent surface coordinates (logical px) and the buffer scale.
  void setGeometry(std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height, std::int32_t scale);
  // Moves the video below another window surface (picture in picture) without restarting playback.
  bool reparent(wl_surface* parent);

private:
  void attach(wl_surface* parent);
  bool createEglSurface();
  void detach();
  static void onMpvUpdate(void* data);
  static void onFrameDone(void* data, wl_callback* callback, std::uint32_t time);
  void scheduleRender();
  void render();

  WaylandConnection& m_wayland;
  GlSharedContext& m_gl;
  MpvPlayer& m_player;
  wl_surface* m_surface = nullptr;
  wl_subsurface* m_subsurface = nullptr;
  wl_egl_window* m_window = nullptr;
  EGLSurface m_eglSurface = EGL_NO_SURFACE;
  EGLConfig m_config = nullptr;
  EGLContext m_context = EGL_NO_CONTEXT;
  mpv_render_context* m_render = nullptr;
  wl_callback* m_frame = nullptr;
  bool m_dirty = false;
  bool m_renderQueued = false;
  std::uint64_t m_frames = 0;
  std::int32_t m_width = 0;
  std::int32_t m_height = 0;
  std::int32_t m_scale = 1;
  std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
  struct UpdateToken {
    std::weak_ptr<bool> alive;
    VideoSurface* self;
  };
  std::unique_ptr<UpdateToken> m_token; // mpv's update callback data; freed after the render context
};
