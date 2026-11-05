#include "video_surface.h"

#include "core/deferred_call.h"
#include "core/log.h"
#include "mpv_lib.h"
#include "mpv_player.h"
#include "render/gl_shared_context.h"
#include "wayland/wayland_connection.h"

#include <wayland-client.h>
#include <wayland-egl.h>

namespace {
  constexpr Logger kLog("video");

  void* procAddress(void* /*ctx*/, const char* name) { return reinterpret_cast<void*>(eglGetProcAddress(name)); }

  // libdesk-ui code assumes a current context belongs to its share group: put the previous one back.
  class CurrentContextScope {
  public:
    explicit CurrentContextScope(EGLDisplay display)
        : m_display(display), m_context(eglGetCurrentContext()), m_draw(eglGetCurrentSurface(EGL_DRAW)),
          m_read(eglGetCurrentSurface(EGL_READ)) {}
    ~CurrentContextScope() { eglMakeCurrent(m_display, m_draw, m_read, m_context); }
    CurrentContextScope(const CurrentContextScope&) = delete;
    CurrentContextScope& operator=(const CurrentContextScope&) = delete;

  private:
    EGLDisplay m_display;
    EGLContext m_context;
    EGLSurface m_draw;
    EGLSurface m_read;
  };
} // namespace

VideoSurface::VideoSurface(WaylandConnection& wayland, GlSharedContext& gl, MpvPlayer& player)
    : m_wayland(wayland), m_gl(gl), m_player(player) {}

VideoSurface::~VideoSurface() {
  *m_alive = false;
  const EGLDisplay display = m_gl.display();
  const CurrentContextScope restore(display);
  if (m_render != nullptr) {
    eglMakeCurrent(display, m_eglSurface, m_eglSurface, m_context);
    m_player.lib()->renderSetUpdateCallback(m_render, nullptr, nullptr);
    m_player.lib()->renderFree(m_render);
  }
  m_token.reset();
  if (m_context != EGL_NO_CONTEXT) {
    eglDestroyContext(display, m_context);
  }
  detach();
}

bool VideoSurface::initialize(wl_surface* parent) {
  if (m_render != nullptr) {
    return true;
  }
  if (!m_player.ready() || m_wayland.compositor() == nullptr || m_wayland.subcompositor() == nullptr) {
    return false;
  }
  attach(parent);
  const EGLDisplay display = m_gl.display();
  const CurrentContextScope restore(display);
  // Exactly R8G8B8 with no alpha: an XRGB8888 buffer, which the compositor can scan out or put on an overlay plane.
  const EGLint configAttribs[] = {EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                                  EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 0, EGL_NONE};
  EGLConfig configs[64];
  EGLint count = 0;
  EGLConfig config = nullptr;
  eglChooseConfig(display, configAttribs, configs, 64, &count);
  for (EGLint i = 0; i < count && config == nullptr; ++i) {
    EGLint r = 0, g = 0, b = 0, a = 0;
    eglGetConfigAttrib(display, configs[i], EGL_RED_SIZE, &r);
    eglGetConfigAttrib(display, configs[i], EGL_GREEN_SIZE, &g);
    eglGetConfigAttrib(display, configs[i], EGL_BLUE_SIZE, &b);
    eglGetConfigAttrib(display, configs[i], EGL_ALPHA_SIZE, &a);
    if (r == 8 && g == 8 && b == 8 && a == 0) {
      config = configs[i];
    }
  }
  if (config == nullptr) {
    kLog.warn("no XRGB8888 EGL config; the video uses the window config");
    config = m_gl.config();
  }
  m_config = config;
  if (!createEglSurface()) {
    return false;
  }
  // mpv draws only in this context: no sharing with the UI contexts.
  eglBindAPI(EGL_OPENGL_ES_API);
  const EGLint contextAttribs[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_NONE};
  m_context = eglCreateContext(display, config, EGL_NO_CONTEXT, contextAttribs);
  if (m_context == EGL_NO_CONTEXT) {
    const EGLint es2[] = {EGL_CONTEXT_MAJOR_VERSION, 2, EGL_NONE};
    m_context = eglCreateContext(display, config, EGL_NO_CONTEXT, es2);
  }
  if (m_context == EGL_NO_CONTEXT) {
    kLog.warn("video eglCreateContext failed (0x{:04x})", eglGetError());
    return false;
  }
  if (eglMakeCurrent(display, m_eglSurface, m_eglSurface, m_context) != EGL_TRUE) {
    kLog.warn("video eglMakeCurrent failed (0x{:04x})", eglGetError());
    return false;
  }
  // Frames are paced by wl_surface.frame, not by a blocking swap.
  eglSwapInterval(display, 0);

  mpv_opengl_init_params gl{.get_proc_address = procAddress, .get_proc_address_ctx = nullptr};
  mpv_render_param params[] = {
      {MPV_RENDER_PARAM_API_TYPE, const_cast<char*>(MPV_RENDER_API_TYPE_OPENGL)},
      {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &gl},
      {MPV_RENDER_PARAM_WL_DISPLAY, m_wayland.display()},
      {MPV_RENDER_PARAM_INVALID, nullptr},
  };
  if (const int err = m_player.lib()->renderCreate(&m_render, m_player.handle(), params); err < 0) {
    kLog.warn("mpv render context: {}", m_player.lib()->errorString(err));
    m_render = nullptr;
    return false;
  }
  m_token = std::make_unique<UpdateToken>(UpdateToken{.alive = m_alive, .self = this});
  m_player.lib()->renderSetUpdateCallback(m_render, onMpvUpdate, m_token.get());
  return true;
}

void VideoSurface::attach(wl_surface* parent) {
  m_surface = wl_compositor_create_surface(m_wayland.compositor());
  m_subsurface = wl_subcompositor_get_subsurface(m_wayland.subcompositor(), m_surface, parent);
  wl_subsurface_place_below(m_subsurface, parent);
  wl_subsurface_set_desync(m_subsurface);
  // Input goes to the window surface above: an empty input region lets it through.
  wl_region* empty = wl_compositor_create_region(m_wayland.compositor());
  wl_surface_set_input_region(m_surface, empty);
  wl_region_destroy(empty);

  m_window = wl_egl_window_create(m_surface, 16, 16);
}

bool VideoSurface::createEglSurface() {
  m_eglSurface = eglCreateWindowSurface(m_gl.display(), m_config, reinterpret_cast<EGLNativeWindowType>(m_window), nullptr);
  if (m_eglSurface == EGL_NO_SURFACE) {
    kLog.warn("video eglCreateWindowSurface failed (0x{:04x})", eglGetError());
    return false;
  }
  return true;
}

void VideoSurface::detach() {
  if (m_frame != nullptr) {
    wl_callback_destroy(m_frame);
    m_frame = nullptr;
  }
  if (m_eglSurface != EGL_NO_SURFACE) {
    eglDestroySurface(m_gl.display(), m_eglSurface);
    m_eglSurface = EGL_NO_SURFACE;
  }
  if (m_window != nullptr) {
    wl_egl_window_destroy(m_window);
    m_window = nullptr;
  }
  if (m_subsurface != nullptr) {
    wl_subsurface_destroy(m_subsurface);
    m_subsurface = nullptr;
  }
  if (m_surface != nullptr) {
    wl_surface_destroy(m_surface);
    m_surface = nullptr;
  }
  m_width = m_height = 0;
}

bool VideoSurface::reparent(wl_surface* parent) {
  if (m_render == nullptr) {
    return false;
  }
  // The GL context and mpv's render context stay: only the Wayland and EGL surfaces move, so playback goes on.
  const CurrentContextScope restore(m_gl.display());
  eglMakeCurrent(m_gl.display(), EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  detach();
  attach(parent);
  if (!createEglSurface()) {
    return false;
  }
  m_dirty = true;
  kLog.debug("video moved to a new window");
  return true;
}

void VideoSurface::onMpvUpdate(void* data) {
  // mpv thread: only hop to the loop (DeferredCall is thread-safe and wakes it).
  const auto& token = *static_cast<const UpdateToken*>(data);
  DeferredCall::callLater([alive = token.alive, self = token.self] {
    if (!alive.expired()) {
      self->scheduleRender();
    }
  });
}

void VideoSurface::setGeometry(std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height, std::int32_t scale) {
  if (m_subsurface == nullptr) {
    return;
  }
  wl_subsurface_set_position(m_subsurface, x, y);
  kLog.debug("video at {},{} {}x{} scale {}", x, y, width, height, scale);
  if (width != m_width || height != m_height || scale != m_scale) {
    m_width = width;
    m_height = height;
    m_scale = scale;
    wl_egl_window_resize(m_window, width * scale, height * scale, 0, 0);
    wl_surface_set_buffer_scale(m_surface, scale);
    wl_region* opaque = wl_compositor_create_region(m_wayland.compositor());
    wl_region_add(opaque, 0, 0, width, height);
    wl_surface_set_opaque_region(m_surface, opaque);
    wl_region_destroy(opaque);
    m_dirty = true;
    scheduleRender();
  }
}

void VideoSurface::scheduleRender() {
  if (m_renderQueued || m_frame != nullptr) {
    return;
  }
  m_renderQueued = true;
  DeferredCall::callLater([this, alive = std::weak_ptr<bool>(m_alive)] {
    if (alive.expired()) {
      return;
    }
    m_renderQueued = false;
    render();
  });
}

void VideoSurface::render() {
  if (m_render == nullptr || m_width <= 0 || m_height <= 0) {
    return;
  }
  const MpvLib& lib = *m_player.lib();
  if ((lib.renderUpdate(m_render) & MPV_RENDER_UPDATE_FRAME) == 0 && !m_dirty) {
    return;
  }
  m_dirty = false;
  const EGLDisplay display = m_gl.display();
  const CurrentContextScope restore(display);
  eglMakeCurrent(display, m_eglSurface, m_eglSurface, m_context);
  mpv_opengl_fbo fbo{.fbo = 0, .w = m_width * m_scale, .h = m_height * m_scale, .internal_format = 0};
  int flip = 1;
  mpv_render_param params[] = {
      {MPV_RENDER_PARAM_OPENGL_FBO, &fbo},
      {MPV_RENDER_PARAM_FLIP_Y, &flip},
      {MPV_RENDER_PARAM_INVALID, nullptr},
  };
  lib.render(m_render, params);
  if (m_frames++ == 0) {
    kLog.debug("first video frame {}x{}", fbo.w, fbo.h);
  }
  static const wl_callback_listener listener{.done = onFrameDone};
  m_frame = wl_surface_frame(m_surface);
  wl_callback_add_listener(m_frame, &listener, this);
  if (eglSwapBuffers(display, m_eglSurface) != EGL_TRUE) {
    // Nothing was committed, so the frame callback would never come and rendering would stop for good.
    kLog.warn("video swap failed (0x{:04x})", eglGetError());
    wl_callback_destroy(m_frame);
    m_frame = nullptr;
    m_dirty = true;
    return;
  }
  lib.renderReportSwap(m_render);
}

void VideoSurface::onFrameDone(void* data, wl_callback* callback, std::uint32_t /*time*/) {
  auto* self = static_cast<VideoSurface*>(data);
  wl_callback_destroy(callback);
  self->m_frame = nullptr;
  self->scheduleRender();
}
