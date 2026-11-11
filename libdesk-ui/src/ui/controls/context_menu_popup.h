#pragma once

#include "ui/shadow_style.h"
#include "render/animation/animation_manager.h"
#include "render/scene/input_dispatcher.h"
#include "ui/controls/context_menu.h"
#include "ui/controls/scroll_view.h"
#include "ui/popup_chrome.h"
#include "ui/popup_parent.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

class Node;
class PopupSurface;
class RenderContext;
class WaylandConnection;
struct KeyboardEvent;
struct PointerEvent;
struct wl_surface;
class ContextMenuPopupTestAccess;

struct ContextMenuPopupPlacement {
  std::uint32_t anchor = 0;
  std::uint32_t gravity = 0;
  std::int32_t offsetX = 0;
  std::int32_t offsetY = 0;
  popup_chrome::Attachment chromeAttachment{
      .horizontal = popup_chrome::HorizontalAttachment::Center,
      .vertical = popup_chrome::VerticalAttachment::Top,
  };
};

struct ContextMenuPopupRequest {
  std::vector<ContextMenuControlEntry> entries;
  // <= 0 sizes the menu to its widest entry, clamped to [minMenuWidth, maxMenuWidth] (0 = unbounded).
  float menuWidth = 0.0F;
  float minMenuWidth = 0.0F;
  float maxMenuWidth = 0.0F;
  // Scales row heights, fonts, and insets (select-style dropdowns derive this from their font size).
  float contentScale = 1.0F;
  std::size_t maxVisible = 0;
  // Entry highlighted (and scrolled into view) when the menu opens; out-of-range falls back to the
  // first interactive entry.
  std::size_t initialHighlight = static_cast<std::size_t>(-1);
  PopupAnchorRect anchor;
  PopupSurfaceParent parent;
  // Surface whose pointer events are translated into menu coordinates while a press holds the
  // pointer capture (scrollbar thumb drags leaving the popup). Defaults to parent.wlSurface.
  wl_surface* pointerParentSurface = nullptr;
  // Serial of the input event that requested this popup. When absent, legacy
  // callers retain the previous last-input-serial behavior.
  std::optional<std::uint32_t> inputSerial = std::nullopt;
  std::optional<ContextMenuPopupPlacement> placement = std::nullopt;
  // Grow from the anchor (P10). False when a menu bar switches menus while tracking: they swap at once, as on macOS.
  bool grow = true;
};

class ContextMenuPopup {
public:
  ContextMenuPopup(WaylandConnection& wayland, RenderContext& renderContext);
  ~ContextMenuPopup();

  void open(ContextMenuPopupRequest request);
  void close();
  [[nodiscard]] bool isOpen() const noexcept;

  void setOnActivate(std::function<void(const ContextMenuControlEntry&)> callback);
  void setOnDismissed(std::function<void()> callback);
  // Left (-1) / Right (+1) while open, for menu bars that move to the next menu; runs after the key handler returns.
  void setOnNavigate(std::function<void(int)> callback);
  void setShadowConfig(const ShadowStyle& shadow);

  bool onPointerEvent(const PointerEvent& event);
  void onKeyboardEvent(const KeyboardEvent& event);
  [[nodiscard]] wl_surface* wlSurface() const noexcept;
  // This menu's surface or an open submenu's, at any depth: pointer events on either belong to this menu.
  [[nodiscard]] bool ownsSurface(wl_surface* surface) const noexcept;
  [[nodiscard]] xdg_surface* xdgSurface() const noexcept;
  [[nodiscard]] std::uint32_t width() const noexcept;
  [[nodiscard]] std::uint32_t height() const noexcept;

  // Route a keyboard event to the currently-open context menu, if any. A grab
  // popup is modal, so while one is open it swallows keys (returns true).
  static bool dispatchKeyboardEvent(const KeyboardEvent& event);

private:
  friend class ContextMenuPopupTestAccess;

  WaylandConnection& m_wayland;
  RenderContext& m_renderContext;
  std::unique_ptr<PopupSurface> m_surface;
  std::unique_ptr<Node> m_sceneRoot;
  InputDispatcher m_inputDispatcher;
  ScrollViewState m_scrollState{};
  ScrollView* m_scrollView = nullptr;
  ContextMenuControl* m_menu = nullptr;
  std::size_t m_highlightedIndex = 0;
  wl_surface* m_wlSurface = nullptr;
  wl_surface* m_pointerParentSurface = nullptr;
  bool m_pointerInside = false;
  bool m_closing = false; // fading out after an activation: input is ignored
  std::function<void()> m_pendingAction; // runs when the fade ends or the menu closes first

  void restoreParentKeyboardInteractivity();
  // Opens the children of row `index` in a nested grab popup beside the row; closes any other submenu first.
  void openSubmenu(std::size_t index);
  void closeSubmenu();
  void ensureHighlightedVisible();
  void requestVisualUpdate();
  void deferActivation(ContextMenuControlEntry entry);
  void fadeOut(std::function<void()> then);
  // Grab popups close at once: during a fade the grab would keep the keys and block a second grabbing popup.
  void deferClose();
  void applyTransform();

  AnimationManager m_animations;
  AnimationManager::Id m_scaleAnim = 0;
  AnimationManager::Id m_opacityAnim = 0;
  float m_scale = 1.0F;
  float m_opacity = 1.0F;
  float m_originX = 0.0F;
  float m_originY = 0.0F;

  std::function<void(const ContextMenuControlEntry&)> m_onActivate;
  std::function<void()> m_onDismissed;
  std::function<void(int)> m_onNavigate;
  ShadowStyle m_shadowConfig;
  // Deferred popup callbacks must not dereference this after an owner (for
  // example, a plugin panel being unregistered) destroys the popup.
  std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);

  // Parent layer surface (e.g. a bar) whose keyboard interactivity is flipped to
  // OnDemand while the menu is open so the grabbing popup inherits keyboard
  // focus, then restored to None on close.
  zwlr_layer_surface_v1* m_keyboardParentLayerSurface = nullptr;
  wl_surface* m_keyboardParentWlSurface = nullptr;

  std::vector<ContextMenuControlEntry> m_entries;
  float m_contentScale = 1.0F;
  float m_contentX = 0.0F;
  float m_contentY = 0.0F;
  float m_contentWidth = 0.0F;
  wl_output* m_output = nullptr;
  std::unique_ptr<ContextMenuPopup> m_child;
  std::size_t m_childIndex = static_cast<std::size_t>(-1);
  ContextMenuPopup* m_parentMenu = nullptr;
  PaletteTone m_tone = PaletteTone::Scheme;

  static ContextMenuPopup* s_openMenu;
};
