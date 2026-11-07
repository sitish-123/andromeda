#pragma once

#include "compositors/wayland_toplevels.h"
#include "core/timer_manager.h"
#include "render/scene/input_dispatcher.h"
#include "system/icon_resolver.h"
#include "ui/popup_chrome.h"
#include "ui/popup_parent.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

class CompositorPlatform;
class ConfigService;
class Node;
class PopupSurface;
class RenderContext;
class ToplevelThumbnailCapture;
struct PointerEvent;
struct ScreencopyImage;
struct wl_output;
struct wl_surface;
struct zwlr_layer_surface_v1;

// Window previews over a taskbar button (DESIGN.md "Design review fixes" R6): a glass popover with one card per window
// of the app, each a header (app icon, title, close on hover) over a snapshot that refreshes while the popover is open.
// A click on a card focuses that window. The popover takes no grab, so the pointer moves freely between it and the bar.
class TaskbarPreview {
public:
  struct Request {
    zwlr_layer_surface_v1* parent = nullptr;
    wl_output* output = nullptr;
    // The button, in the parent surface's coordinates.
    PopupAnchorRect anchor;
    std::string barPosition;
    // Identifies the button the preview belongs to.
    std::string key;
    std::vector<ToplevelInfo> windows;
    ResolvedIcon icon;
    float contentScale = 1.0F;
  };

  TaskbarPreview(CompositorPlatform& platform, ConfigService& config, RenderContext& renderContext);
  ~TaskbarPreview();
  TaskbarPreview(const TaskbarPreview&) = delete;
  TaskbarPreview& operator=(const TaskbarPreview&) = delete;

  void open(Request request);
  void close();
  [[nodiscard]] bool isOpen() const noexcept { return m_surface != nullptr; }
  [[nodiscard]] const std::string& key() const noexcept { return m_request.key; }
  [[nodiscard]] bool pointerInside() const noexcept { return m_pointerInside; }
  // Called when the pointer leaves the popover.
  void setOnPointerLeave(std::function<void()> callback) { m_onPointerLeave = std::move(callback); }
  // Routes a pointer event to the popover; returns whether it was consumed.
  bool onPointerEvent(const PointerEvent& event);

private:
  struct Card {
    ToplevelInfo window;
    std::shared_ptr<const ScreencopyImage> snapshot;
  };
  void build();
  void captureNext();
  // Rebuilds the scene on the next frame; never inside an input callback, which runs on a node of the scene.
  void requestRebuild();

  CompositorPlatform& m_platform;
  ConfigService& m_config;
  RenderContext& m_renderContext;
  std::unique_ptr<ToplevelThumbnailCapture> m_capture;
  std::unique_ptr<PopupSurface> m_surface;
  std::unique_ptr<Node> m_sceneRoot;
  popup_chrome::Geometry m_chrome;
  InputDispatcher m_input;
  Request m_request;
  std::vector<Card> m_cards;
  std::size_t m_nextCapture = 0;
  std::size_t m_hovered = 0;
  Timer m_refresh;
  std::function<void()> m_onPointerLeave;
  bool m_pointerInside = false;
  bool m_hasHover = false;
  bool m_dirty = false;
};
