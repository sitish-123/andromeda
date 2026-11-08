#include "ui/controls/context_menu_popup.h"

#include "core/deferred_call.h"
#include "core/input/key_symbols.h"
#include "core/input/keybind_matcher.h"
#include "core/log.h"
#include "core/ui_phase.h"
#include "render/render_context.h"
#include "render/render_target.h"
#include "render/scene/node.h"
#include "ui/controls/scroll_view.h"
#include "ui/popup_chrome.h"
#include "ui/style.h"
#include "wayland/layer_surface.h"
#include "wayland/popup_surface.h"
#include "wayland/wayland_connection.h"
#include "wayland/wayland_seat.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <wayland-client-protocol.h>

namespace {

  constexpr Logger kLog("context-menu-popup");

} // namespace

ContextMenuPopup* ContextMenuPopup::s_openMenu = nullptr;

ContextMenuPopup::ContextMenuPopup(WaylandConnection& wayland, RenderContext& renderContext)
    : m_wayland(wayland), m_renderContext(renderContext) {}

ContextMenuPopup::~ContextMenuPopup() {
  if (m_alive != nullptr) {
    *m_alive = false;
  }
  close();
}

void ContextMenuPopup::open(ContextMenuPopupRequest request) {
  close();

  // maxVisible caps the popup viewport; all entries remain reachable via scroll.
  const std::size_t maxVisible =
      request.maxVisible > 0 ? request.maxVisible : std::max<std::size_t>(1, request.entries.size());
  const float contentScale = std::max(0.1F, request.contentScale);
  const float menuHeight = ContextMenuControl::preferredHeight(request.entries, maxVisible, contentScale);
  float menuWidth = request.menuWidth;
  if (menuWidth <= 0.0F) {
    float measureScale = 1.0F;
    if (request.parent.output != nullptr) {
      if (const WaylandOutput* out = m_wayland.findOutputByWl(request.parent.output); out != nullptr) {
        measureScale = out->configuredScale();
      }
    }
    ScaledRenderer measureRenderer(m_renderContext, measureScale);
    menuWidth = ContextMenuControl::preferredWidth(measureRenderer, request.entries, contentScale);
    if (request.maxMenuWidth > 0.0F) {
      menuWidth = std::min(menuWidth, request.maxMenuWidth);
    }
    if (request.minMenuWidth > 0.0F) {
      menuWidth = std::max(menuWidth, request.minMenuWidth);
    }
  }
  const auto chrome =
      popup_chrome::computeGeometry(menuWidth, menuHeight, m_shadowConfig, Style::popupShadowsEnabled());
  m_contentScale = contentScale;
  m_contentX = chrome.contentX();
  m_contentY = chrome.contentY();
  m_contentWidth = chrome.contentWidth;
  m_output = request.parent.output;
  m_scrollState = {};
  m_scrollView = nullptr;
  m_menu = nullptr;
  m_highlightedIndex = request.initialHighlight < request.entries.size() ? request.initialHighlight : 0;

  const ContextMenuPopupPlacement defaultPlacement{
      .anchor = XDG_POSITIONER_ANCHOR_BOTTOM,
      .gravity = XDG_POSITIONER_GRAVITY_BOTTOM,
      .offsetX = 0,
      .offsetY = static_cast<std::int32_t>(Style::spaceXs),
      .chromeAttachment = popup_chrome::Attachment{
          .horizontal = popup_chrome::HorizontalAttachment::Center, .vertical = popup_chrome::VerticalAttachment::Top
      },
  };
  const ContextMenuPopupPlacement resolvedPlacement = request.placement.value_or(defaultPlacement);

  PopupSurfaceConfig popupCfg{
      .anchorX = request.anchor.x,
      .anchorY = request.anchor.y,
      .anchorWidth = std::max(1, request.anchor.width),
      .anchorHeight = std::max(1, request.anchor.height),
      .width = chrome.surfaceWidth,
      .height = chrome.surfaceHeight,
      .anchor = resolvedPlacement.anchor,
      .gravity = resolvedPlacement.gravity,
      .constraintAdjustment = XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X
          | XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y
          | XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_X
          | XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y,
      .offsetX = resolvedPlacement.offsetX,
      .offsetY = resolvedPlacement.offsetY,
      .serial = request.inputSerial.value_or(m_wayland.lastInputSerial()),
      .grab = true,
  };
  popup_chrome::applyToConfig(popupCfg, chrome, resolvedPlacement.chromeAttachment);

  m_surface = std::make_unique<PopupSurface>(m_wayland);
  m_surface->setRenderContext(&m_renderContext);
  m_surface->setAnimationManager(&m_animations);

  // The anchor corner of the card: Grow starts there and Return goes back to it.
  const auto& attach = resolvedPlacement.chromeAttachment;
  m_originX = chrome.contentX()
      + (attach.horizontal == popup_chrome::HorizontalAttachment::Left    ? 0.0F
             : attach.horizontal == popup_chrome::HorizontalAttachment::Right ? chrome.contentWidth
                                                                              : chrome.contentWidth * 0.5F);
  m_originY = chrome.contentY()
      + (attach.vertical == popup_chrome::VerticalAttachment::Top      ? 0.0F
             : attach.vertical == popup_chrome::VerticalAttachment::Bottom ? chrome.contentHeight
                                                                           : chrome.contentHeight * 0.5F);
  m_scale = request.grow ? 0.92F : 1.0F;
  m_opacity = request.grow ? 0.0F : 1.0F;
  if (request.grow) {
    // Reduced motion: the scale jumps (spatial) and only the fade runs.
    m_scaleAnim = m_animations.animateSpatial(
        m_scale, 1.0F, Style::spring(Tokens::springOpen),
        [this](float v) {
          m_scale = v;
          applyTransform();
        },
        [this] { m_scaleAnim = 0; }
    );
    m_opacityAnim = m_animations.animateSpring(
        m_opacity, 1.0F, Style::spring(Tokens::springFade),
        [this](float v) {
          m_opacity = v;
          applyTransform();
        },
        [this] { m_opacityAnim = 0; }
    );
  }

  auto* self = this;

  m_surface->setConfigureCallback([self](std::uint32_t /*w*/, std::uint32_t /*h*/) {
    self->m_surface->requestLayout();
  });

  m_entries = std::move(request.entries);
  m_surface->setPrepareFrameCallback([self, chrome, contentScale](bool /*needsUpdate*/, bool needsLayout) {
    if (self->m_surface == nullptr) {
      return;
    }

    const auto width = self->m_surface->width();
    const auto height = self->m_surface->height();
    if (width == 0 || height == 0) {
      return;
    }

    self->m_renderContext.makeCurrent(self->m_surface->renderTarget());

    const bool needsSceneBuild = self->m_sceneRoot == nullptr
        || static_cast<std::uint32_t>(std::round(self->m_sceneRoot->width())) != width
        || static_cast<std::uint32_t>(std::round(self->m_sceneRoot->height())) != height;
    if (!needsSceneBuild && !needsLayout) {
      return;
    }

    UiPhaseScope layoutPhase(UiPhase::Layout);

    if (self->m_menu != nullptr) {
      self->m_highlightedIndex = self->m_menu->highlightedIndex();
    }

    const auto fw = static_cast<float>(width);
    const auto fh = static_cast<float>(height);

    self->m_sceneRoot = std::make_unique<Node>();
    self->m_sceneRoot->setSize(fw, fh);
    self->m_sceneRoot->setTransformOrigin(self->m_originX, self->m_originY);
    self->m_sceneRoot->setScale(self->m_scale);
    self->m_sceneRoot->setOpacity(self->m_opacity);
    if (Style::popupShadowsEnabled()) {
      (void)popup_chrome::addShadow(
          *self->m_sceneRoot, chrome, self->m_shadowConfig, Style::scaledRadiusLg(contentScale)
      );
    }
    (void)popup_chrome::addCardBackground(*self->m_sceneRoot, chrome, contentScale);
    if (self->m_surface != nullptr) {
      popup_chrome::setCardBlurRegion(*self->m_surface, chrome, contentScale);
    }

    auto scrollView = std::make_unique<ScrollView>();
    scrollView->setPosition(chrome.contentX(), chrome.contentY());
    scrollView->setSize(chrome.contentWidth, chrome.contentHeight);
    scrollView->setContentScale(contentScale);
    scrollView->setViewportPaddingH(0.0F);
    scrollView->setViewportPaddingV(0.0F);
    scrollView->clearFill();
    scrollView->clearBorder();
    scrollView->setRadius(0.0F);
    scrollView->bindState(&self->m_scrollState);
    scrollView->setScrollbarVisible(true);
    scrollView->setScrollbarInsetV(Style::scaledRadiusLg(contentScale));

    auto ctrl = std::make_unique<ContextMenuControl>();
    ContextMenuControl* menuPtr = ctrl.get();
    ctrl->setContentScale(contentScale);
    ctrl->setMenuWidth(chrome.contentWidth);
    // Lay out every entry; ScrollView clips to maxVisible viewport height.
    ctrl->setMaxVisible(self->m_entries.size());
    ctrl->setEntries(self->m_entries);
    ctrl->setHighlightedIndex(self->m_highlightedIndex);
    self->m_highlightedIndex = ctrl->highlightedIndex();
    ctrl->setRedrawCallback([self] {
      if (self->m_surface)
        self->m_surface->requestRedraw();
    });
    ctrl->setOnActivate([self](const ContextMenuControlEntry& e) { self->deferActivation(e); });
    ctrl->setOnSubmenuOpen([self](const ContextMenuControlEntry& /*entry*/, float /*rowCenterY*/) {
      if (self->m_menu != nullptr) {
        self->openSubmenu(self->m_menu->highlightedIndex());
      }
    });
    scrollView->content()->addChild(std::move(ctrl));
    ScrollView* scrollPtr = scrollView.get();
    scrollView->layout(self->m_surface->renderTarget().renderer());

    self->m_scrollView = scrollPtr;
    self->m_menu = menuPtr;
    self->ensureHighlightedVisible();

    self->m_sceneRoot->addChild(std::move(scrollView));
    self->m_inputDispatcher.setSceneRoot(self->m_sceneRoot.get());
    self->m_inputDispatcher.setCursorShapeCallback([self](std::uint32_t serial, std::uint32_t shape) {
      self->m_wayland.setCursorShape(serial, shape);
    });
    self->m_surface->setSceneRoot(self->m_sceneRoot.get());
    // The Grow springs started before the surface was configured: make sure the frame loop ticks them.
    if (self->m_animations.hasActive()) {
      self->m_surface->requestFrameTick();
    }
  });

  m_surface->setDismissedCallback([self] { self->deferClose(); });

  // Layer-shell popups inherit their parent's keyboard interactivity. A bar is
  // None, so flip it to OnDemand before the popup maps or the grabbing popup
  // gets no keyboard focus and ESC cannot reach it. Only bar-style callers pass
  // a parent wlSurface; panels are already OnDemand and leave it null.
  if (request.parent.layerSurface != nullptr && request.parent.wlSurface != nullptr) {
    m_keyboardParentLayerSurface = request.parent.layerSurface;
    m_keyboardParentWlSurface = request.parent.wlSurface;
    zwlr_layer_surface_v1_set_keyboard_interactivity(
        request.parent.layerSurface, static_cast<std::uint32_t>(LayerShellKeyboard::OnDemand)
    );
    wl_surface_commit(request.parent.wlSurface);
  }

  const bool initialized = request.parent.xdgSurface != nullptr
      ? m_surface->initializeAsChild(request.parent.xdgSurface, request.parent.output, popupCfg)
      : m_surface->initialize(request.parent.layerSurface, request.parent.output, popupCfg);
  if (!initialized) {
    kLog.warn("failed to create context menu popup");
    restoreParentKeyboardInteractivity();
    m_surface.reset();
    return;
  }

  popup_chrome::setContentInputRegion(*m_surface, chrome);
  m_wlSurface = m_surface->wlSurface();
  m_pointerParentSurface =
      request.pointerParentSurface != nullptr ? request.pointerParentSurface : request.parent.wlSurface;
  s_openMenu = this;
}

void ContextMenuPopup::close() {
  const bool wasOpen = m_surface != nullptr;
  m_animations.cancelAll();
  m_scaleAnim = m_opacityAnim = 0;
  // xdg_popup: the topmost popup goes first.
  closeSubmenu();
  if (s_openMenu == this) {
    s_openMenu = m_parentMenu != nullptr && m_parentMenu->isOpen() ? m_parentMenu : nullptr;
  }
  restoreParentKeyboardInteractivity();
  m_menu = nullptr;
  m_scrollView = nullptr;
  m_sceneRoot.reset();
  m_surface.reset();
  m_wlSurface = nullptr;
  m_pointerParentSurface = nullptr;
  m_pointerInside = false;
  if (wasOpen && m_onDismissed) {
    m_onDismissed();
  }
}

bool ContextMenuPopup::isOpen() const noexcept { return m_surface != nullptr; }

void ContextMenuPopup::applyTransform() {
  if (m_sceneRoot != nullptr) {
    m_sceneRoot->setScale(m_scale);
    m_sceneRoot->setOpacity(m_opacity);
  }
  if (m_surface != nullptr) {
    m_surface->requestRedraw();
  }
}

void ContextMenuPopup::deferActivation(ContextMenuControlEntry entry) {
  auto* self = this;
  const std::weak_ptr<bool> alive = m_alive;
  auto onActivate = m_onActivate;
  DeferredCall::callLater([self, alive, onActivate = std::move(onActivate), entry = std::move(entry)] {
    const auto token = alive.lock();
    if (token == nullptr || !*token) {
      return;
    }
    // Close before running the action. The action may open another popup (e.g. the
    // color picker for a "Custom" entry); that popup must be created against the
    // now-topmost parent, and this menu cannot be destroyed while it still has a
    // child popup on top. Activating first violates the xdg_popup topmost rule.
    self->close();
    if (onActivate) {
      onActivate(entry);
    }
  });
}

void ContextMenuPopup::deferClose() {
  auto* self = this;
  const std::weak_ptr<bool> alive = m_alive;
  DeferredCall::callLater([self, alive] {
    const auto token = alive.lock();
    if (token != nullptr && *token) {
      self->close();
    }
  });
}

void ContextMenuPopup::setOnActivate(std::function<void(const ContextMenuControlEntry&)> callback) {
  m_onActivate = std::move(callback);
}

void ContextMenuPopup::setOnDismissed(std::function<void()> callback) { m_onDismissed = std::move(callback); }

void ContextMenuPopup::setOnNavigate(std::function<void(int)> callback) { m_onNavigate = std::move(callback); }

void ContextMenuPopup::setShadowConfig(const ShadowStyle& shadow) {
  if (m_shadowConfig == shadow) {
    return;
  }
  m_shadowConfig = shadow;
  if (isOpen()) {
    close();
  }
}

bool ContextMenuPopup::onPointerEvent(const PointerEvent& event) {
  if (!isOpen()) {
    return false;
  }
  if (m_child != nullptr && m_child->isOpen() && m_child->onPointerEvent(event)) {
    return true;
  }

  const bool captured = m_inputDispatcher.pointerCaptured();
  const bool onPopup = (event.surface != nullptr && event.surface == m_wlSurface);
  auto localX = static_cast<float>(event.sx);
  auto localY = static_cast<float>(event.sy);
  // While a press holds the pointer capture (e.g. a scrollbar thumb drag), events sliding onto the
  // parent surface are translated into popup-local coordinates so the drag keeps tracking instead
  // of being cut off at the popup edge.
  bool mapped = onPopup;
  if (!onPopup
      && captured
      && m_surface != nullptr
      && event.surface != nullptr
      && event.surface == m_pointerParentSurface) {
    localX -= static_cast<float>(m_surface->configuredX());
    localY -= static_cast<float>(m_surface->configuredY());
    mapped = true;
  }

  switch (event.type) {
  case PointerEvent::Type::Enter:
    if (onPopup) {
      m_pointerInside = true;
      m_inputDispatcher.pointerEnter(localX, localY, event.serial);
    } else if (mapped) {
      // Parent-surface enter during a captured drag: swallow it so the parent's hover
      // states don't react while the drag owns the pointer.
      return true;
    }
    break;
  case PointerEvent::Type::Leave:
    if (onPopup) {
      m_pointerInside = false;
      // A captured drag survives leaving the surface; the deferred leave is delivered on release.
      if (!captured) {
        m_inputDispatcher.pointerLeave();
      }
    }
    break;
  case PointerEvent::Type::Motion:
    if (mapped || m_pointerInside) {
      if (onPopup) {
        m_pointerInside = true;
      }
      m_inputDispatcher.pointerMotion(localX, localY, 0);
      // macOS: resting on a submenu row opens it, another row closes it.
      if (onPopup && m_menu != nullptr && m_menu->highlightedIndex() != m_childIndex) {
        const std::size_t index = m_menu->highlightedIndex();
        if (index < m_entries.size() && !m_entries[index].children.empty()) {
          openSubmenu(index);
        } else {
          closeSubmenu();
        }
      }
      return true;
    }
    break;
  case PointerEvent::Type::Button:
    if (mapped || m_pointerInside) {
      if (onPopup) {
        m_pointerInside = true;
      }
      const bool pressed = event.pressed;
      m_inputDispatcher.pointerButton(localX, localY, event.button, pressed, event.serial, event.time, event.touch);
      if (!pressed && captured && !onPopup) {
        m_pointerInside = false;
        m_inputDispatcher.pointerLeave();
      }
      return true;
    }
    break;
  case PointerEvent::Type::Axis:
    if (onPopup || m_pointerInside) {
      if (onPopup) {
        m_pointerInside = true;
      }
      const bool consumed = m_inputDispatcher.pointerAxis(
          localX, localY, event.axis, event.axisSource, event.axisValue, event.axisDiscrete, event.axisValue120,
          event.axisLines
      );
      if (m_surface != nullptr && m_sceneRoot != nullptr) {
        if (m_sceneRoot->layoutDirty()) {
          m_surface->requestLayout();
        } else if (m_sceneRoot->paintDirty() || consumed) {
          m_surface->requestRedraw();
        }
      }
      return consumed || onPopup;
    }
    break;
  }

  if (m_surface != nullptr && m_sceneRoot != nullptr && m_surface->isRunning()) {
    m_surface->requestRedraw();
  }

  return onPopup;
}

void ContextMenuPopup::onKeyboardEvent(const KeyboardEvent& event) {
  if (!isOpen() || !event.pressed || event.preedit) {
    return;
  }

  const std::uint32_t sym = event.sym;
  const std::uint32_t modifiers = event.modifiers;

  if (KeybindMatcher::matches(KeybindAction::Cancel, sym, modifiers)) {
    deferClose();
    return;
  }

  if (m_menu == nullptr) {
    return;
  }

  const bool left = KeybindMatcher::matches(KeybindAction::Left, sym, modifiers);
  if (left && m_parentMenu != nullptr) {
    deferClose();
    return;
  }
  if (KeybindMatcher::matches(KeybindAction::Right, sym, modifiers) && m_menu->highlightedIndex() < m_entries.size()
      && !m_entries[m_menu->highlightedIndex()].children.empty()) {
    openSubmenu(m_menu->highlightedIndex());
    return;
  }
  if (m_onNavigate && (left || KeybindMatcher::matches(KeybindAction::Right, sym, modifiers))) {
    const std::weak_ptr<bool> alive = m_alive;
    DeferredCall::callLater([alive, onNavigate = m_onNavigate, delta = left ? -1 : 1] {
      const auto token = alive.lock();
      if (token != nullptr && *token) {
        onNavigate(delta);
      }
    });
    return;
  }

  if (KeybindMatcher::matches(KeybindAction::Down, sym, modifiers)) {
    if (m_menu->moveHighlight(1)) {
      m_highlightedIndex = m_menu->highlightedIndex();
      ensureHighlightedVisible();
      requestVisualUpdate();
    }
  } else if (KeybindMatcher::matches(KeybindAction::Up, sym, modifiers)) {
    if (m_menu->moveHighlight(-1)) {
      m_highlightedIndex = m_menu->highlightedIndex();
      ensureHighlightedVisible();
      requestVisualUpdate();
    }
  } else if (KeybindMatcher::matches(KeybindAction::Validate, sym, modifiers)) {
    (void)m_menu->activateHighlighted();
  } else if (KeySymbol::isHome(sym)) {
    m_menu->setHighlightedIndex(0);
    m_highlightedIndex = m_menu->highlightedIndex();
    ensureHighlightedVisible();
    requestVisualUpdate();
  } else if (KeySymbol::isEnd(sym) && m_menu->entryCount() > 0) {
    m_menu->setHighlightedIndex(m_menu->entryCount() - 1);
    m_highlightedIndex = m_menu->highlightedIndex();
    ensureHighlightedVisible();
    requestVisualUpdate();
  }
}

void ContextMenuPopup::ensureHighlightedVisible() {
  if (m_menu == nullptr || m_scrollView == nullptr || !m_scrollView->scrollable()) {
    return;
  }
  const std::size_t index = m_menu->highlightedIndex();
  const float rowTop = m_menu->rowTop(index);
  const float rowBottom = m_menu->rowBottom(index);
  const float viewportH = m_scrollView->contentViewportHeight();
  const float offset = m_scrollView->scrollOffset();
  if (rowTop < offset) {
    m_scrollView->setScrollOffset(rowTop);
  } else if (rowBottom > offset + viewportH) {
    m_scrollView->setScrollOffset(rowBottom - viewportH);
  }
}

void ContextMenuPopup::requestVisualUpdate() {
  if (m_surface == nullptr || m_sceneRoot == nullptr) {
    return;
  }
  if (m_sceneRoot->layoutDirty()) {
    m_surface->requestLayout();
  } else {
    m_surface->requestRedraw();
  }
}

void ContextMenuPopup::restoreParentKeyboardInteractivity() {
  zwlr_layer_surface_v1* layerSurface = m_keyboardParentLayerSurface;
  wl_surface* wlSurface = m_keyboardParentWlSurface;
  m_keyboardParentLayerSurface = nullptr;
  m_keyboardParentWlSurface = nullptr;
  // The parent surface can be destroyed while the menu is open (bar rebuild,
  // output removal). Its proxies are then freed, so only touch it while still registered.
  if (layerSurface == nullptr || m_wayland.layerSurfaceFor(wlSurface) != layerSurface) {
    return;
  }
  zwlr_layer_surface_v1_set_keyboard_interactivity(layerSurface, static_cast<std::uint32_t>(LayerShellKeyboard::None));
  wl_surface_commit(wlSurface);
}

void ContextMenuPopup::openSubmenu(std::size_t index) {
  if (m_menu == nullptr || m_surface == nullptr || index >= m_entries.size() || m_entries[index].children.empty()) {
    return;
  }
  if (m_child != nullptr && m_child->isOpen() && m_childIndex == index) {
    return;
  }
  closeSubmenu();
  const float offset = m_scrollView != nullptr ? m_scrollView->scrollOffset() : 0.0F;
  const float top = m_contentY + m_menu->rowTop(index) - offset;
  const float bottom = m_contentY + m_menu->rowBottom(index) - offset;
  m_child = std::make_unique<ContextMenuPopup>(m_wayland, m_renderContext);
  m_child->m_parentMenu = this;
  m_child->setShadowConfig(m_shadowConfig);
  const std::weak_ptr<bool> alive = m_alive;
  m_child->setOnActivate([this, alive](const ContextMenuControlEntry& entry) {
    const auto token = alive.lock();
    if (token != nullptr && *token) {
      deferActivation(entry);
    }
  });
  // A submenu closed on its own (Left, dismiss) lets the same row open it again on the next hover.
  m_child->setOnDismissed([this, alive] {
    const auto token = alive.lock();
    if (token != nullptr && *token) {
      m_childIndex = static_cast<std::size_t>(-1);
    }
  });
  m_childIndex = index;
  m_child->open({
      .entries = m_entries[index].children,
      .contentScale = m_contentScale,
      .anchor = {
          .x = static_cast<std::int32_t>(m_contentX),
          .y = static_cast<std::int32_t>(top),
          .width = static_cast<std::int32_t>(m_contentWidth),
          .height = std::max(1, static_cast<std::int32_t>(bottom - top)),
      },
      .parent = {.xdgSurface = xdgSurface(), .output = m_output},
      // The submenu's first row lines up with this row: its top edge sits one menu padding above the row.
      .placement = ContextMenuPopupPlacement{
          .anchor = XDG_POSITIONER_ANCHOR_TOP_RIGHT,
          .gravity = XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT,
          .offsetX = 0,
          .offsetY = -static_cast<std::int32_t>(Style::spaceXs * m_contentScale),
          .chromeAttachment = {.horizontal = popup_chrome::HorizontalAttachment::Left,
                               .vertical = popup_chrome::VerticalAttachment::Top},
      },
  });
}

void ContextMenuPopup::closeSubmenu() {
  m_childIndex = static_cast<std::size_t>(-1);
  if (m_child != nullptr) {
    m_child->close();
  }
}

bool ContextMenuPopup::dispatchKeyboardEvent(const KeyboardEvent& event) {
  if (s_openMenu == nullptr || !s_openMenu->isOpen()) {
    return false;
  }
  s_openMenu->onKeyboardEvent(event);
  return true;
}

wl_surface* ContextMenuPopup::wlSurface() const noexcept { return m_wlSurface; }

xdg_surface* ContextMenuPopup::xdgSurface() const noexcept {
  return m_surface != nullptr ? m_surface->xdgSurface() : nullptr;
}

std::uint32_t ContextMenuPopup::width() const noexcept { return m_surface != nullptr ? m_surface->width() : 0; }

std::uint32_t ContextMenuPopup::height() const noexcept { return m_surface != nullptr ? m_surface->height() : 0; }
