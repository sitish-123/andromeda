#include "wayland/toplevel_surface.h"

#include "core/log.h"
#include "wayland/wayland_connection.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#include <algorithm>
#include <utility>

namespace {

  constexpr Logger kLog("wayland");

  const xdg_surface_listener kXdgSurfaceListener = {
      .configure = &ToplevelSurface::handleXdgSurfaceConfigure,
  };

  const xdg_toplevel_listener kToplevelListener = {
      .configure = &ToplevelSurface::handleToplevelConfigure,
      .close = &ToplevelSurface::handleToplevelClose,
      .configure_bounds = &ToplevelSurface::handleToplevelConfigureBounds,
      .wm_capabilities = &ToplevelSurface::handleToplevelWmCapabilities,
  };

} // namespace

ToplevelSurface::ToplevelSurface(WaylandConnection& connection) : Surface(connection) {}

ToplevelSurface::~ToplevelSurface() {
  m_connection.unregisterSurface(m_surface);
  destroyRoleObjects();
}

bool ToplevelSurface::initialize(wl_output* output, const ToplevelSurfaceConfig& config) {
  if (!m_connection.hasXdgShell()) {
    kLog.warn("toplevel: missing xdg-shell");
    return false;
  }

  if (!createWlSurface()) {
    return false;
  }

  std::int32_t bufferScale = 1;
  if (output != nullptr) {
    if (const auto* wlOutput = m_connection.findOutputByWl(output); wlOutput != nullptr) {
      bufferScale = wlOutput->scale;
    }
    m_connection.registerSurfaceOutput(m_surface, output);
  }
  setBufferScale(bufferScale);

  m_pendingWidth = std::max<std::uint32_t>(1, config.width);
  m_pendingHeight = std::max<std::uint32_t>(1, config.height);
  m_floatingWidth = m_pendingWidth;
  m_resizeMargin = config.resizeMargin;
  m_floatingHeight = m_pendingHeight;
  m_lastToplevelWidth = 0;
  m_lastToplevelHeight = 0;

  m_xdgSurface = xdg_wm_base_get_xdg_surface(m_connection.xdgWmBase(), m_surface);
  if (m_xdgSurface == nullptr) {
    destroySurface();
    return false;
  }
  xdg_surface_add_listener(m_xdgSurface, &kXdgSurfaceListener, this);

  m_toplevel = xdg_surface_get_toplevel(m_xdgSurface);
  if (m_toplevel == nullptr) {
    destroyRoleObjects();
    destroySurface();
    return false;
  }

  xdg_toplevel_add_listener(m_toplevel, &kToplevelListener, this);
  // The window draws its own header, so the compositor must not add a title bar.
  if (auto* manager = m_connection.decorationManager(); manager != nullptr) {
    m_decoration = zxdg_decoration_manager_v1_get_toplevel_decoration(manager, m_toplevel);
    zxdg_toplevel_decoration_v1_set_mode(m_decoration, ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE);
  }
  xdg_toplevel_set_title(m_toplevel, config.title.empty() ? "Noctalia" : config.title.c_str());
  setAccessibleTitle(config.title);
  xdg_toplevel_set_app_id(m_toplevel, config.appId != nullptr ? config.appId : "dev.noctalia.Noctalia");
  if (config.minWidth > 0 || config.minHeight > 0) {
    xdg_toplevel_set_min_size(
        m_toplevel, static_cast<std::int32_t>(config.minWidth), static_cast<std::int32_t>(config.minHeight)
    );
  }

  if (config.maximized) {
    xdg_toplevel_set_maximized(m_toplevel);
  }

  wl_surface_commit(m_surface);
  wl_display_flush(m_connection.display());
  if (wl_display_roundtrip(m_connection.display()) < 0) {
    kLog.warn("toplevel: initial roundtrip failed");
    destroyRoleObjects();
    destroySurface();
    return false;
  }

  setRunning(true);
  return true;
}

void ToplevelSurface::setClosedCallback(std::function<void()> callback) { m_closedCallback = std::move(callback); }

void ToplevelSurface::setMinSize(std::uint32_t minWidth, std::uint32_t minHeight) {
  if (m_toplevel != nullptr) {
    xdg_toplevel_set_min_size(m_toplevel, static_cast<std::int32_t>(minWidth), static_cast<std::int32_t>(minHeight));
  }
}

void ToplevelSurface::beginMove(std::uint32_t serial) {
  if (m_toplevel != nullptr) {
    xdg_toplevel_move(m_toplevel, m_connection.seat(), serial);
  }
}

void ToplevelSurface::beginResize(std::uint32_t serial, std::uint32_t edges) {
  if (m_toplevel != nullptr) {
    xdg_toplevel_resize(m_toplevel, m_connection.seat(), serial, edges);
  }
}

void ToplevelSurface::setMaximized(bool maximized) {
  if (m_toplevel == nullptr) {
    return;
  }
  if (maximized) {
    xdg_toplevel_set_maximized(m_toplevel);
  } else {
    xdg_toplevel_unset_maximized(m_toplevel);
  }
}

void ToplevelSurface::setFullscreen(bool fullscreen) {
  if (m_toplevel == nullptr) {
    return;
  }
  if (fullscreen) {
    xdg_toplevel_set_fullscreen(m_toplevel, nullptr);
  } else {
    xdg_toplevel_unset_fullscreen(m_toplevel);
  }
}

void ToplevelSurface::setMinimized() {
  if (m_toplevel != nullptr) {
    xdg_toplevel_set_minimized(m_toplevel);
  }
}

void ToplevelSurface::showWindowMenu(std::uint32_t serial, std::int32_t x, std::int32_t y) {
  if (m_toplevel != nullptr) {
    xdg_toplevel_show_window_menu(m_toplevel, m_connection.seat(), serial, x, y);
  }
}

bool ToplevelSurface::requestWindowSize(std::uint32_t width, std::uint32_t height) {
  if (m_xdgSurface == nullptr || m_windowWidth == 0 || m_state.maximized || m_state.fullscreen ||
      m_lastToplevelWidth > 0 || m_lastToplevelHeight > 0) {
    return false;
  }
  width = std::max<std::uint32_t>(1, width);
  height = std::max<std::uint32_t>(1, height);
  m_floatingWidth = width;
  m_floatingHeight = height;
  m_windowWidth = width;
  m_windowHeight = height;
  if (m_resizeMargin > 0) {
    const auto m = static_cast<std::int32_t>(m_margin);
    xdg_surface_set_window_geometry(m_xdgSurface, m, m, static_cast<std::int32_t>(width), static_cast<std::int32_t>(height));
  }
  Surface::onConfigure(width + 2 * m_margin, height + 2 * m_margin);
  return true;
}

void ToplevelSurface::handleXdgSurfaceConfigure(void* data, xdg_surface* surface, std::uint32_t serial) {
  auto* self = static_cast<ToplevelSurface*>(data);
  xdg_surface_ack_configure(surface, serial);
  const ToplevelState& next = self->m_pendingState;
  // Tiled edges alone do not mean a layout-tiled window: some compositors send them to floating windows.
  const bool floating = !next.maximized && !next.fullscreen;

  std::uint32_t width = self->m_pendingWidth;
  std::uint32_t height = self->m_pendingHeight;
  // 0 x 0 = the client picks. After an unmaximize that is the last floating size, not the maximized one.
  if (floating && self->m_lastToplevelWidth <= 0 && self->m_floatingWidth > 0) {
    width = self->m_floatingWidth;
  }
  if (floating && self->m_lastToplevelHeight <= 0 && self->m_floatingHeight > 0) {
    height = self->m_floatingHeight;
  }
  if (self->m_lastToplevelWidth > 0) {
    width = static_cast<std::uint32_t>(self->m_lastToplevelWidth);
  }
  if (self->m_lastToplevelHeight > 0) {
    height = static_cast<std::uint32_t>(self->m_lastToplevelHeight);
  }
  width = std::max<std::uint32_t>(1, width);
  height = std::max<std::uint32_t>(1, height);
  if (floating) {
    self->m_floatingWidth = width;
    self->m_floatingHeight = height;
  }
  const bool stateChanged = next != self->m_state;
  self->m_state = next;
  self->m_margin = floating ? self->m_resizeMargin : 0;
  self->m_windowWidth = width;
  self->m_windowHeight = height;
  if (self->m_resizeMargin > 0) {
    const auto m = static_cast<std::int32_t>(self->m_margin);
    xdg_surface_set_window_geometry(
        surface, m, m, static_cast<std::int32_t>(width), static_cast<std::int32_t>(height)
    );
  }
  self->Surface::onConfigure(width + 2 * self->m_margin, height + 2 * self->m_margin);
  // After onConfigure, so the callback reads the size of this configure.
  if (stateChanged && self->m_stateCallback) {
    self->m_stateCallback(self->m_state);
  }
}

void ToplevelSurface::handleToplevelConfigure(
    void* data, xdg_toplevel* /*toplevel*/, std::int32_t width, std::int32_t height, wl_array* states
) {
  auto* self = static_cast<ToplevelSurface*>(data);
  ToplevelState next;
  if (states != nullptr) {
    const auto* begin = static_cast<const std::uint32_t*>(states->data);
    for (std::size_t i = 0; i < states->size / sizeof(std::uint32_t); ++i) {
      switch (begin[i]) {
      case XDG_TOPLEVEL_STATE_MAXIMIZED:
        next.maximized = true;
        break;
      case XDG_TOPLEVEL_STATE_FULLSCREEN:
        next.fullscreen = true;
        break;
      case XDG_TOPLEVEL_STATE_ACTIVATED:
        next.activated = true;
        break;
      case XDG_TOPLEVEL_STATE_TILED_LEFT:
      case XDG_TOPLEVEL_STATE_TILED_RIGHT:
      case XDG_TOPLEVEL_STATE_TILED_TOP:
      case XDG_TOPLEVEL_STATE_TILED_BOTTOM:
        next.tiled = true;
        break;
      default:
        break;
      }
    }
  }
  self->m_pendingState = next;
  self->m_lastToplevelWidth = width;
  self->m_lastToplevelHeight = height;
  if (width > 0) {
    self->m_pendingWidth = static_cast<std::uint32_t>(width);
  }
  if (height > 0) {
    self->m_pendingHeight = static_cast<std::uint32_t>(height);
  }
}

void ToplevelSurface::handleToplevelClose(void* data, xdg_toplevel* /*toplevel*/) {
  auto* self = static_cast<ToplevelSurface*>(data);
  self->setRunning(false);
  if (self->m_closedCallback) {
    self->m_closedCallback();
  }
}

void ToplevelSurface::handleToplevelConfigureBounds(
    void* /*data*/, xdg_toplevel* /*toplevel*/, std::int32_t /*width*/, std::int32_t /*height*/
) {}

void ToplevelSurface::
    handleToplevelWmCapabilities(void* /*data*/, xdg_toplevel* /*toplevel*/, wl_array* /*capabilities*/) {}

void ToplevelSurface::destroyRoleObjects() {
  if (m_decoration != nullptr) {
    zxdg_toplevel_decoration_v1_destroy(m_decoration);
    m_decoration = nullptr;
  }
  if (m_toplevel != nullptr) {
    xdg_toplevel_destroy(m_toplevel);
    m_toplevel = nullptr;
  }
  if (m_xdgSurface != nullptr) {
    xdg_surface_destroy(m_xdgSurface);
    m_xdgSurface = nullptr;
  }
}
