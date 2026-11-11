#pragma once

#include "compositors/wayland_toplevels.h"
#include "render/animation/animation_manager.h"
#include "core/timer_manager.h"
#include "render/scene/input_dispatcher.h"
#include "system/icon_resolver.h"
#include "ui/popup_chrome.h"
#include "ui/popup_parent.h"
#include "ui/signal.h"

#include <array>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

class Box;
class CompositorPlatform;
class ConfigService;
class Node;
class PopupSurface;
class RenderContext;
class ToplevelThumbnailCapture;
struct PointerEvent;
struct PopupSurfaceConfig;
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
    // Icon and display name for a snap-group member's app id.
    std::function<std::pair<ResolvedIcon, std::string>(const std::string& appId)> describeApp;
    float contentScale = 1.0F;
  };

  TaskbarPreview(CompositorPlatform& platform, ConfigService& config, RenderContext& renderContext);
  ~TaskbarPreview();
  TaskbarPreview(const TaskbarPreview&) = delete;
  TaskbarPreview& operator=(const TaskbarPreview&) = delete;

  // Opening while shown or closing keeps the surface: it moves to the new button at once (board section 2).
  void open(Request request);
  // Return (P10): fade_out with scale toward the button, then the surface goes.
  void close();
  // No animation: for teardown, when the parent surface goes away.
  void closeNow();
  [[nodiscard]] bool isOpen() const noexcept { return m_surface != nullptr && !m_closing; }
  [[nodiscard]] const std::string& key() const noexcept { return m_request.key; }
  [[nodiscard]] const std::vector<ToplevelInfo>& windows() const noexcept { return m_request.windows; }
  [[nodiscard]] bool pointerInside() const noexcept { return m_pointerInside; }
  // Called when the pointer leaves the popover.
  void setOnPointerLeave(std::function<void()> callback) { m_onPointerLeave = std::move(callback); }
  // Routes a pointer event to the popover; returns whether it was consumed.
  bool onPointerEvent(const PointerEvent& event);

private:
  // The snap group one of the windows belongs to, shown as one card first (board desktop-menus.html B).
  struct GroupMember {
    ToplevelInfo window;
    // Zone as fractions of the work area (x, y, w, h).
    std::array<float, 4> zone{};
    ResolvedIcon icon;
    std::string name;
  };
  std::vector<GroupMember> m_group;
  // Group members first (all drawn in slot 0), then the app's own windows (one slot each).
  struct Card {
    ToplevelInfo window;
    std::shared_ptr<const ScreencopyImage> snapshot;
  };
  [[nodiscard]] bool hasGroup() const noexcept { return !m_group.empty(); }
  [[nodiscard]] std::size_t slotCount() const noexcept { return m_cards.size() - m_group.size() + (hasGroup() ? 1 : 0); }
  [[nodiscard]] float slotX(std::size_t slot) const;
  void build();
  void captureNext();
  [[nodiscard]] int cardW() const;
  [[nodiscard]] int cardH() const;
  // Rebuilds the scene on the next frame; never inside an input callback, which runs on a node of the scene.
  void requestRebuild();
  [[nodiscard]] PopupSurfaceConfig surfaceConfig();
  void driveShown(bool shown);
  void applyTransform();
  void moveLens(std::size_t slot);
  void applyLens();
  // Runs `fn` on the next main-loop turn unless this preview is gone by then.
  void defer(std::function<void()> fn);

  CompositorPlatform& m_platform;
  ConfigService& m_config;
  RenderContext& m_renderContext;
  std::unique_ptr<ToplevelThumbnailCapture> m_capture;
  std::unique_ptr<PopupSurface> m_surface;
  std::unique_ptr<Node> m_sceneRoot;
  Signal<>::ScopedConnection m_glassConn;
  popup_chrome::Geometry m_chrome;
  InputDispatcher m_input;
  Request m_request;
  std::vector<Card> m_cards;
  std::size_t m_nextCapture = 0;
  std::size_t m_hovered = 0;
  int m_thumbW = 0;
  int m_thumbH = 0;
  Timer m_refresh;
  std::function<void()> m_onPointerLeave;
  bool m_pointerInside = false;
  bool m_hasHover = false;
  bool m_dirty = false;
  bool m_buttonHeld = false;
  // Grow / Return (P10) on the scene root, and the lens edges (P1); kept across scene rebuilds.
  AnimationManager m_animations;
  AnimationManager::Id m_scaleAnim = 0;
  AnimationManager::Id m_opacityAnim = 0;
  AnimationManager::Id m_lensLeadAnim = 0;
  AnimationManager::Id m_lensTrailAnim = 0;
  AnimationManager::Id m_lensFadeAnim = 0;
  float m_scale = 1.0F;
  float m_opacity = 1.0F;
  float m_lensLeft = 0.0F;
  float m_lensRight = 0.0F;
  float m_lensOpacity = 1.0F;
  bool m_closing = false;
  Box* m_lens = nullptr;
  std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};
