#pragma once

#include "render/animation/animation_manager.h"
#include "render/scene/input_dispatcher.h"
#include "render/scene/node.h"
#include "wayland/toplevel_surface.h"

#include <memory>
#include <optional>
#include <string>

class Box;
class ConfigService;
class RenderContext;
class SelectDropdownPopup;
class ShellWaylandConnection;
class WallpaperPanel;
struct KeyboardEvent;
struct LayerPopupParentContext;
struct PointerEvent;
struct wl_output;
struct wl_surface;

// The wallpaper picker as a normal app window (xdg toplevel) with the Settings glass look. It hosts WallpaperPanel's
// content; the panel lives as long as this object and builds its scene at each open.
class WallpaperWindow {
public:
  WallpaperWindow();
  ~WallpaperWindow();

  void initialize(
      ShellWaylandConnection& wayland, ConfigService* config, RenderContext* renderContext,
      std::unique_ptr<WallpaperPanel> panel
  );

  void open(wl_output* output = nullptr);
  void toggle(wl_output* output = nullptr);
  void close();
  // Closes the window and drops the panel while the services it uses still live.
  void shutdown();
  [[nodiscard]] bool isOpen() const noexcept { return m_surface != nullptr && m_surface->isRunning(); }

  // Panel-side requests: a full layout, an update only, or keyboard focus.
  void refresh();
  void requestUpdateOnly();
  void setFocus(InputArea* area);
  [[nodiscard]] InputArea* focusedArea() const noexcept;

  [[nodiscard]] bool ownsKeyboardSurface(wl_surface* surface) const noexcept;
  [[nodiscard]] bool onPointerEvent(const PointerEvent& event);
  void onKeyboardEvent(const KeyboardEvent& event);
  void onThemeChanged();
  [[nodiscard]] std::optional<LayerPopupParentContext> popupParentContextForSurface(wl_surface* surface) const;
  [[nodiscard]] std::optional<LayerPopupParentContext> fallbackPopupParentContext() const;

private:
  void destroyWindow();
  void prepareFrame(bool needsUpdate, bool needsLayout);
  void buildScene(float width, float height);
  void syncBlur();
  void invalidate();
  [[nodiscard]] float uiScale() const;

  ShellWaylandConnection* m_wayland = nullptr;
  ConfigService* m_config = nullptr;
  RenderContext* m_renderContext = nullptr;
  std::unique_ptr<WallpaperPanel> m_panel;
  std::unique_ptr<ToplevelSurface> m_surface;
  std::unique_ptr<SelectDropdownPopup> m_selectPopup;
  std::unique_ptr<Node> m_sceneRoot;
  Box* m_background = nullptr;
  Node* m_content = nullptr;
  AnimationManager m_animations;
  InputDispatcher m_inputDispatcher;
  wl_output* m_output = nullptr;
  bool m_pointerInside = false;
  bool m_glass = false;
  bool m_rebuildRequested = false;
};
