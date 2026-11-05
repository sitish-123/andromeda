#pragma once

#include "wayland/surface.h"

#include <cstdint>
#include <functional>
#include <string>

struct wl_array;
struct wl_output;
struct xdg_surface;
struct xdg_toplevel;

struct ToplevelSurfaceConfig {
  std::uint32_t width = 720;
  std::uint32_t height = 480;
  std::uint32_t minWidth = 0;
  std::uint32_t minHeight = 0;
  std::string title = "Noctalia";
  const char* appId = "dev.noctalia.Noctalia";
  bool maximized = false; // asked before the first commit, so the first configure is already maximized
  // Transparent band around the window, outside its xdg window geometry, for client-side resize (the GTK way).
  // Only while the window is free: maximized and fullscreen windows have none.
  std::uint32_t resizeMargin = 0;
};

// xdg_toplevel states from the last configure.
struct ToplevelState {
  bool maximized = false;
  bool fullscreen = false;
  bool activated = false;
  bool tiled = false; // any tiled edge
  bool operator==(const ToplevelState&) const = default;
};

class ToplevelSurface : public Surface {
public:
  explicit ToplevelSurface(WaylandConnection& connection);
  ~ToplevelSurface() override;

  using Surface::initialize;
  bool initialize() override { return false; }
  bool initialize(wl_output* output, const ToplevelSurfaceConfig& config);

  void setClosedCallback(std::function<void()> callback);
  void setMinSize(std::uint32_t minWidth, std::uint32_t minHeight);
  void beginMove(std::uint32_t serial);
  void beginResize(std::uint32_t serial, std::uint32_t edges);
  void setMaximized(bool maximized);
  void setMinimized();
  void showWindowMenu(std::uint32_t serial, std::int32_t x, std::int32_t y);
  // Called after a configure whose states differ from the last one.
  void setStateCallback(std::function<void(const ToplevelState&)> callback) { m_stateCallback = std::move(callback); }
  [[nodiscard]] const ToplevelState& state() const noexcept { return m_state; }
  // The window inside the surface: the surface is the window plus margin() on every side.
  [[nodiscard]] std::uint32_t margin() const noexcept { return m_margin; }
  [[nodiscard]] std::uint32_t windowWidth() const noexcept { return m_windowWidth; }
  [[nodiscard]] std::uint32_t windowHeight() const noexcept { return m_windowHeight; }

  [[nodiscard]] xdg_surface* xdgSurface() const noexcept { return m_xdgSurface; }

  static void handleXdgSurfaceConfigure(void* data, xdg_surface* surface, std::uint32_t serial);
  static void handleToplevelConfigure(
      void* data, xdg_toplevel* toplevel, std::int32_t width, std::int32_t height, wl_array* states
  );
  static void handleToplevelClose(void* data, xdg_toplevel* toplevel);
  static void
  handleToplevelConfigureBounds(void* data, xdg_toplevel* toplevel, std::int32_t width, std::int32_t height);
  static void handleToplevelWmCapabilities(void* data, xdg_toplevel* toplevel, wl_array* capabilities);

private:
  void destroyRoleObjects();

  xdg_surface* m_xdgSurface = nullptr;
  xdg_toplevel* m_toplevel = nullptr;
  std::function<void()> m_closedCallback;
  std::function<void(const ToplevelState&)> m_stateCallback;
  ToplevelState m_state;
  ToplevelState m_pendingState;
  std::uint32_t m_floatingWidth = 0; // last size with no maximized or fullscreen state
  std::uint32_t m_floatingHeight = 0;
  std::uint32_t m_resizeMargin = 0;
  std::uint32_t m_margin = 0;
  std::uint32_t m_windowWidth = 0;
  std::uint32_t m_windowHeight = 0;
  std::uint32_t m_pendingWidth = 0;
  std::uint32_t m_pendingHeight = 0;
  std::int32_t m_lastToplevelWidth = 0;
  std::int32_t m_lastToplevelHeight = 0;
};
