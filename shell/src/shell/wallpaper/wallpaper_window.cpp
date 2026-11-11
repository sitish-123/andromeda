#include "shell/wallpaper/wallpaper_window.h"

#include "compositors/shell_wayland_connection.h"
#include "config/config_service.h"
#include "core/deferred_call.h"
#include "core/input/key_modifiers.h"
#include "core/log.h"
#include "i18n/i18n.h"
#include "render/render_context.h"
#include "shell/settings/settings_selection_style.h"
#include "shell/tooltip/tooltip_manager.h"
#include "shell/wallpaper/panel/wallpaper_panel.h"
#include "ui/builders.h"
#include "ui/controls/box.h"
#include "ui/controls/select_dropdown_popup.h"
#include "ui/dialogs/layer_popup_host.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "wayland/wayland_seat.h"

#include <algorithm>
#include <cmath>
#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace {

  constexpr Logger kLog("wallpaper-window");

  constexpr float kWindowWidth = 980.0F;
  constexpr float kWindowHeight = 700.0F;
  constexpr float kWindowMinWidth = 560.0F;
  constexpr float kWindowMinHeight = 440.0F;

} // namespace

WallpaperWindow::WallpaperWindow() = default;

WallpaperWindow::~WallpaperWindow() { destroyWindow(); }

void WallpaperWindow::initialize(
    ShellWaylandConnection& wayland, ConfigService* config, RenderContext* renderContext,
    std::unique_ptr<WallpaperPanel> panel
) {
  m_wayland = &wayland;
  m_config = config;
  m_renderContext = renderContext;
  m_panel = std::move(panel);
  m_panel->setWindow(this);
}

float WallpaperWindow::uiScale() const {
  return m_config != nullptr ? std::max(0.1F, m_config->config().accessibility.uiScale) : 1.0F;
}

void WallpaperWindow::toggle(wl_output* output) {
  if (isOpen()) {
    close();
  } else {
    open(output);
  }
}

void WallpaperWindow::open(wl_output* output) {
  if (m_wayland == nullptr || m_renderContext == nullptr || m_panel == nullptr || !m_wayland->hasXdgShell()) {
    return;
  }
  if (isOpen()) {
    return;
  }
  if (m_surface != nullptr) {
    destroyWindow(); // closed by the compositor, deferred teardown not run yet: finish it before a new surface
  }
  TooltipManager::instance().forceDestroy();

  if (output == nullptr) {
    output = m_wayland->lastPointerOutput();
  }
  if (output == nullptr && !m_wayland->outputs().empty()) {
    output = m_wayland->outputs().front().output;
  }
  m_output = output;

  m_surface = std::make_unique<ToplevelSurface>(*m_wayland);
  m_surface->setRenderContext(m_renderContext);
  m_surface->setAnimationManager(&m_animations);
  m_surface->setClosedCallback([this, surface = m_surface.get()] {
    DeferredCall::callLater([this, surface] {
      if (m_surface.get() == surface) {
        destroyWindow();
      }
    });
  });
  m_surface->setConfigureCallback([this](std::uint32_t, std::uint32_t) {
    if (m_surface != nullptr) {
      m_surface->requestLayout();
    }
  });
  m_surface->setPrepareFrameCallback([this](bool needsUpdate, bool needsLayout) {
    prepareFrame(needsUpdate, needsLayout);
  });
  m_surface->setUpdateCallback([] {});

  const float scale = uiScale();
  const ToplevelSurfaceConfig cfg{
      .width = static_cast<std::uint32_t>(std::round(kWindowWidth * scale)),
      .height = static_cast<std::uint32_t>(std::round(kWindowHeight * scale)),
      .minWidth = static_cast<std::uint32_t>(std::round(kWindowMinWidth * scale)),
      .minHeight = static_cast<std::uint32_t>(std::round(kWindowMinHeight * scale)),
      .title = i18n::tr("wallpaper.panel.title"),
      .appId = "dev.andromeda.Wallpaper",
  };
  if (!m_surface->initialize(output, cfg)) {
    kLog.warn("failed to create the Wallpaper toplevel");
    m_surface.reset();
    return;
  }
  m_pointerInside = false;
}

void WallpaperWindow::close() { destroyWindow(); }

void WallpaperWindow::shutdown() {
  destroyWindow();
  m_panel.reset();
}

void WallpaperWindow::destroyWindow() {
  if (m_surface == nullptr) {
    return;
  }
  TooltipManager::instance().forceDestroy();
  if (m_sceneRoot != nullptr && m_panel != nullptr) {
    m_panel->onClose();
  }
  m_inputDispatcher.pointerLeave();
  m_inputDispatcher.setSceneRoot(nullptr);
  m_surface->setSceneRoot(nullptr);
  m_background = nullptr;
  m_content = nullptr;
  m_sceneRoot.reset();
  m_selectPopup.reset();
  m_surface.reset();
  m_output = nullptr;
  m_pointerInside = false;
  m_rebuildRequested = false;
}

void WallpaperWindow::refresh() {
  if (m_surface != nullptr) {
    m_surface->requestLayout();
  }
}

void WallpaperWindow::requestUpdateOnly() {
  if (m_surface != nullptr) {
    m_surface->requestUpdate();
  }
}

void WallpaperWindow::setFocus(InputArea* area) {
  m_inputDispatcher.setFocus(area);
  invalidate();
}

InputArea* WallpaperWindow::focusedArea() const noexcept { return m_inputDispatcher.focusedArea(); }

void WallpaperWindow::onThemeChanged() {
  if (isOpen()) {
    m_rebuildRequested = true;
    m_surface->requestLayout();
  }
}

void WallpaperWindow::invalidate() {
  if (m_surface == nullptr || m_sceneRoot == nullptr) {
    return;
  }
  if (m_sceneRoot->layoutDirty()) {
    m_surface->requestLayout();
  } else if (m_sceneRoot->paintDirty()) {
    m_surface->requestRedraw();
  }
}

void WallpaperWindow::prepareFrame(bool needsUpdate, bool needsLayout) {
  if (m_renderContext == nullptr || m_surface == nullptr || m_panel == nullptr) {
    return;
  }
  const auto width = static_cast<float>(m_surface->width());
  const auto height = static_cast<float>(m_surface->height());
  if (width <= 0.0F || height <= 0.0F) {
    return;
  }
  m_renderContext->makeCurrent(m_surface->renderTarget());
  Renderer& renderer = m_surface->renderTarget().renderer();
  if (m_sceneRoot == nullptr || m_rebuildRequested) {
    m_rebuildRequested = false;
    buildScene(width, height);
    needsLayout = true;
  }
  if (needsUpdate) {
    m_panel->update(renderer);
  }
  if (needsLayout || m_sceneRoot->width() != width || m_sceneRoot->height() != height) {
    m_sceneRoot->setSize(width, height);
    m_background->setSize(width, height);
    const float pad = Tokens::spaceP16 * uiScale();
    m_content->setPosition(pad, pad);
    m_panel->layout(renderer, width - (2.0F * pad), height - (2.0F * pad));
    syncBlur();
  }
}

void WallpaperWindow::buildScene(float width, float height) {
  if (m_sceneRoot != nullptr) {
    m_panel->onClose();
  }
  m_inputDispatcher.setSceneRoot(nullptr);
  m_sceneRoot = ui::node({});
  m_sceneRoot->setSize(width, height);
  m_sceneRoot->setAnimationManager(&m_animations);

  const Config& cfg = m_config->config();
  m_glass = cfg.shell.settingsWindowTranslucent && !Style::reduceTransparency();
  auto bg = ui::box({
      .width = width,
      .height = height,
      .configure = [glass = m_glass](Box& box) {
        box.setPanelStyle();
        box.setRadius(0.0F);
        box.setBorder(clearColor(), 0);
        if (!glass) {
          box.setFill(colorSpecFromRole(ColorRole::SolidRegular));
        } else {
          // Same window glass as Settings: never thinner than the Tinted recipe.
          const float tinted = glass::material(glass::Kind::Sheet, 0.5F, false, isLightPalette()).tint;
          box.setFill(
              colorSpecFromRole(panelMaterialRole(), std::max(Style::glassMaterial(glass::Kind::Sheet, PaletteTone::Scheme).tint, tinted))
          );
        }
      },
  });
  m_background = static_cast<Box*>(m_sceneRoot->addChild(std::move(bg)));

  m_selectPopup = std::make_unique<SelectDropdownPopup>(*m_wayland, *m_renderContext);
  m_selectPopup->setShadowConfig(cfg.shell.shadow);
  m_selectPopup->setParent(m_surface->xdgSurface(), m_surface->wlSurface(), m_output);
  m_sceneRoot->setPopupContext(m_selectPopup.get());

  m_panel->setContentScale(uiScale());
  m_panel->setAnimationManager(&m_animations);
  m_panel->setPanelCardOpacity(1.0F);
  m_panel->create();
  m_panel->onOpen({});
  auto content = ui::node({});
  m_content = m_sceneRoot->addChild(std::move(content));
  if (m_panel->root() != nullptr) {
    m_content->addChild(m_panel->releaseRoot());
  }

  m_inputDispatcher.setSceneRoot(m_sceneRoot.get());
  m_inputDispatcher.setTextInputContext(m_surface->wlSurface(), m_wayland->textInputService());
  m_inputDispatcher.setCursorShapeCallback([this](std::uint32_t serial, std::uint32_t shape) {
    m_wayland->setCursorShape(serial, shape);
  });
  m_inputDispatcher.setHoverChangeCallback([this](InputArea* /*old*/, InputArea* next) {
    if (m_surface != nullptr) {
      TooltipManager::instance().onHoverChange(next, m_surface->xdgSurface(), m_output);
    }
  });
  m_surface->setSceneRoot(m_sceneRoot.get());
  if (InputArea* focus = m_panel->initialFocusArea(); focus != nullptr) {
    m_inputDispatcher.setFocus(focus);
  }
}

void WallpaperWindow::syncBlur() {
  if (m_surface == nullptr) {
    return;
  }
  if (!m_glass) {
    m_surface->clearBlurRegion();
    return;
  }
  m_surface->setBlurRegion({InputRect{
      .x = 0,
      .y = 0,
      .width = static_cast<int>(m_surface->width()),
      .height = static_cast<int>(m_surface->height()),
  }});
}

bool WallpaperWindow::ownsKeyboardSurface(wl_surface* surface) const noexcept {
  return isOpen() && surface != nullptr && surface == m_surface->wlSurface();
}

bool WallpaperWindow::onPointerEvent(const PointerEvent& event) {
  if (!isOpen()) {
    return false;
  }
  const bool onThis = event.surface != nullptr && event.surface == m_surface->wlSurface();
  bool consumed = false;
  switch (event.type) {
  case PointerEvent::Type::Enter:
    if (onThis) {
      m_pointerInside = true;
      m_inputDispatcher.pointerEnter(static_cast<float>(event.sx), static_cast<float>(event.sy), event.serial);
      consumed = true;
    }
    break;
  case PointerEvent::Type::Leave:
    if (onThis) {
      m_pointerInside = false;
      m_inputDispatcher.pointerLeave();
      consumed = true;
    }
    break;
  case PointerEvent::Type::Motion:
    if (onThis || m_pointerInside) {
      m_inputDispatcher.pointerMotion(static_cast<float>(event.sx), static_cast<float>(event.sy), event.serial);
      consumed = true;
    }
    break;
  case PointerEvent::Type::Button:
    if (onThis || m_pointerInside) {
      m_inputDispatcher.pointerMotion(static_cast<float>(event.sx), static_cast<float>(event.sy), event.serial);
      // The header row doubles as the title bar: a press on empty space there moves the window.
      const bool inTitleBar = event.sy < settings::metrics::kTitleBarHeight * uiScale();
      if (event.pressed && event.button == BTN_LEFT && inTitleBar && m_inputDispatcher.hoveredArea() == nullptr) {
        m_surface->beginMove(event.serial);
      } else {
        m_inputDispatcher.pointerButton(
            static_cast<float>(event.sx), static_cast<float>(event.sy), event.button, event.pressed, event.serial,
            event.time, event.touch
        );
      }
      consumed = true;
    }
    break;
  case PointerEvent::Type::Axis:
    if (m_pointerInside) {
      m_inputDispatcher.pointerAxis(
          static_cast<float>(event.sx), static_cast<float>(event.sy), event.axis, event.axisSource, event.axisValue,
          event.axisDiscrete, event.axisValue120, event.axisLines
      );
      consumed = true;
    }
    break;
  }
  invalidate();
  return consumed;
}

void WallpaperWindow::onKeyboardEvent(const KeyboardEvent& event) {
  if (!isOpen() || m_panel == nullptr) {
    return;
  }
  if (event.pressed && (event.modifiers & KeyMod::Ctrl) != 0 && (event.sym == XKB_KEY_w || event.sym == XKB_KEY_q)) {
    DeferredCall::callLater([this] { close(); });
    return;
  }
  if (m_panel->handleGlobalKey(event.sym, event.modifiers, event.pressed, event.preedit)) {
    invalidate();
    return;
  }
  if (event.pressed && !event.preedit && event.sym == XKB_KEY_Escape) {
    DeferredCall::callLater([this] { close(); });
    return;
  }
  m_inputDispatcher.keyEvent(event.sym, event.utf32, event.modifiers, event.pressed, event.preedit);
  invalidate();
}

std::optional<LayerPopupParentContext> WallpaperWindow::popupParentContextForSurface(wl_surface* surface) const {
  if (!isOpen() || surface == nullptr || surface != m_surface->wlSurface()) {
    return std::nullopt;
  }
  return fallbackPopupParentContext();
}

std::optional<LayerPopupParentContext> WallpaperWindow::fallbackPopupParentContext() const {
  if (!isOpen()) {
    return std::nullopt;
  }
  return LayerPopupParentContext{
      .surface = m_surface->wlSurface(),
      .layerSurface = nullptr,
      .xdgSurface = m_surface->xdgSurface(),
      .output = m_output,
      .width = m_surface->width(),
      .height = m_surface->height(),
  };
}
