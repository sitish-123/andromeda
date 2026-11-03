#include "shell/lockscreen/lock_editor.h"

#include "auth/pam_authenticator.h"
#include "config/config_service.h"
#include "core/deferred_call.h"
#include "core/log.h"
#include "core/ui_phase.h"
#include "i18n/i18n.h"
#include "render/core/render_styles.h"
#include "render/render_context.h"
#include "render/scene/node.h"
#include "render/scene/rect_node.h"
#include "render/scene/wallpaper_node.h"
#include "shell/lockscreen/lock_screen.h"
#include "shell/lockscreen/lock_surface.h"
#include "shell/wallpaper/wallpaper_geometry.h"
#include "ui/auth/auth_layout.h"
#include "ui/auth/clock_style.h"
#include "ui/controls/label.h"
#include "wayland/wayland_connection.h"
#include "wayland/wayland_seat.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace {

  constexpr Logger kLog("lock-editor");

  // Same as the lock: own images blur here (lock_surface.cpp kOwnImageBlur), 4.5:1 dim clamp.
  constexpr float kOwnImageBlur = 0.03F;

  int indexOf(const auto& names, const std::string& value) {
    for (std::size_t i = 0; i < names.size(); ++i) {
      if (value == names[i]) {
        return static_cast<int>(i);
      }
    }
    return 0;
  }

} // namespace

LockEditor::~LockEditor() { close(); }

void LockEditor::initialize(
    WaylandConnection& wayland, ConfigService* config, RenderContext* renderContext, SharedTextureCache* textureCache,
    LockScreen* lockScreen
) {
  m_wayland = &wayland;
  m_config = config;
  m_renderContext = renderContext;
  m_textureCache = textureCache;
  m_lockScreen = lockScreen;
}

void LockEditor::open() {
  if (isOpen() || m_wayland == nullptr || m_renderContext == nullptr || m_config == nullptr) {
    return;
  }
  const WaylandOutput* target = nullptr;
  const wl_output* pointerOutput = m_wayland->lastPointerOutput();
  for (const auto& output : m_wayland->outputs()) {
    if (!output.done || output.output == nullptr || !output.hasUsableGeometry()) {
      continue;
    }
    if (target == nullptr || output.output == pointerOutput) {
      target = &output;
    }
  }
  if (target == nullptr) {
    return;
  }
  m_connector = target->connectorName;
  m_outputName = target->name;

  const auto& lock = m_config->config().lockscreen;
  m_draft = {};
  m_draft.layout = indexOf(LockEditorBar::kLayoutNames, lock.layout);
  m_draft.theme = indexOf(LockEditorBar::kThemeNames, lock.theme);

  auto config = LayerSurfaceConfig{
      .nameSpace = "noctalia-lock-editor",
      .layer = LayerShellLayer::Overlay,
      .anchor = LayerShellAnchor::Top | LayerShellAnchor::Bottom | LayerShellAnchor::Left | LayerShellAnchor::Right,
      .width = 0,
      .height = 0,
      // Over the bar and the dock: the preview has the lock screen's full geometry.
      .exclusiveZone = -1,
      .keyboard = LayerShellKeyboard::Exclusive,
      .defaultWidth = static_cast<std::uint32_t>(target->effectiveLogicalWidth()),
      .defaultHeight = static_cast<std::uint32_t>(target->effectiveLogicalHeight()),
  };
  m_surface = std::make_unique<LayerSurface>(*m_wayland, std::move(config));
  m_surface->setRenderContext(m_renderContext);
  m_surface->setAnimationManager(&m_animations);
  m_surface->setConfigureCallback([this](std::uint32_t, std::uint32_t) { m_surface->requestLayout(); });
  m_surface->setPrepareFrameCallback([this](bool needsUpdate, bool needsLayout) {
    prepareFrame(needsUpdate, needsLayout);
  });
  // An unplugged output or a compositor close ends the session; the close runs after the callback returns.
  m_surface->setClosedCallback([this] { DeferredCall::callLater([this] { close(); }); });
  if (!m_surface->initialize(target->output)) {
    kLog.warn("failed to open the lock screen editor on {}", m_connector);
    m_surface.reset();
    return;
  }
  startClock();
  kLog.info("opened on {}", m_connector);
}

// One layout at each minute (or second) boundary, as on the lock (LockSurface::startAndromedaClock).
void LockEditor::startClock() {
  const auto& lock = m_config->config().lockscreen;
  const std::int64_t period = lock.clockSeconds || clockStyleById(lock.clockStyle).live ? 1000 : 60000;
  const auto now = std::chrono::system_clock::now();
  const auto into = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % period;
  m_clock.start(std::chrono::milliseconds(period - into + 5), [this] {
    refresh();
    startClock();
  });
}

void LockEditor::close() {
  if (!isOpen()) {
    return;
  }
  m_clock.stop();
  m_animations.cancelAll();
  m_wallpaperLease.reset();
  m_surface->setSceneRoot(nullptr);
  m_root.reset();
  m_wallpaper = nullptr;
  m_dim = nullptr;
  m_themeNotice = nullptr;
  m_surface.reset();
  m_wallpaperPath.clear();
  kLog.info("closed");
}

void LockEditor::prepareFrame(bool needsUpdate, bool needsLayout) {
  if (m_surface == nullptr || m_renderContext == nullptr) {
    return;
  }
  const auto width = static_cast<float>(m_surface->width());
  const auto height = static_cast<float>(m_surface->height());
  if (width <= 0.0F || height <= 0.0F) {
    return;
  }
  m_renderContext->makeCurrent(m_surface->renderTarget());
  const bool build = m_root == nullptr;
  if (build) {
    UiPhaseScope phase(UiPhase::Layout);
    buildScene(width, height);
  }
  if (build || needsUpdate || needsLayout) {
    UiPhaseScope phase(UiPhase::Layout);
    layoutScene(width, height);
  }
}

void LockEditor::buildScene(float width, float height) {
  m_root = std::make_unique<Node>();
  m_root->setSize(width, height);
  auto wallpaper = std::make_unique<WallpaperNode>();
  m_wallpaper = static_cast<WallpaperNode*>(m_root->addChild(std::move(wallpaper)));
  m_wallpaper->setZIndex(0);
  auto dim = std::make_unique<RectNode>();
  dim->setHitTestVisible(false);
  m_dim = static_cast<RectNode*>(m_root->addChild(std::move(dim)));
  m_dim->setZIndex(1);
  m_auth.build(*m_root, m_animations);
  auto notice = std::make_unique<Label>();
  notice->setFontWeight(FontWeight::Medium);
  m_themeNotice = static_cast<Label*>(m_root->addChild(std::move(notice)));
  m_themeNotice->setZIndex(5);
  m_bar.build(*m_root);
  m_surface->setSceneRoot(m_root.get());
}

void LockEditor::applyWallpaper() {
  if (m_lockScreen == nullptr || m_textureCache == nullptr || m_wallpaper == nullptr) {
    return;
  }
  const auto& wallpaperConfig = m_config->config().wallpaper;
  m_wallpaper->setFillMode(wallpaperConfig.fillMode);
  const std::string path = m_lockScreen->wallpaperPathForOutput(m_connector);
  const bool own = m_lockScreen->hasOwnWallpaper(m_connector);
  if (path != m_wallpaperPath || own != m_ownWallpaper) {
    m_wallpaperPath = path;
    m_ownWallpaper = own;
    m_wallpaperLease.reset();
    if (!path.empty()) {
      // The preview has no compositor blur behind it, so every image blurs here as the lock's own image does.
      const float blur = m_config->config().lockscreen.blurIntensity * kOwnImageBlur;
      m_wallpaperLease = m_textureCache->acquire(
          SharedTextureRequest{
              .path = path,
              .coverage = wallpaperTextureCoverage(m_wayland->outputs(), m_outputName, wallpaperConfig.fillMode),
              .blur = blur,
          },
          &m_renderContext->backend(), [this] { refresh(); }
      );
    }
  }
  const TextureHandle texture = m_wallpaperLease.texture();
  m_wallpaper->setVisible(texture.id != 0);
  if (texture.id != 0) {
    m_wallpaper->setTextures(
        texture.id, {}, static_cast<float>(texture.width), static_cast<float>(texture.height), 0.0F, 0.0F
    );
  }
}

void LockEditor::layoutScene(float width, float height) {
  Renderer& renderer = m_surface->renderTarget().renderer();
  const auto& config = m_config->config();
  const float scale = config.accessibility.uiScale;
  m_root->setSize(width, height);
  m_wallpaper->setPosition(0.0F, 0.0F);
  m_wallpaper->setSize(width, height);
  applyWallpaper();

  float dim = std::min(config.lockscreen.tintIntensity, 0.6F);
  const float bright = m_wallpaperLease.texture().brightLuminance;
  if (bright > 0.0F) {
    constexpr float kMaxBackground = 1.05F / 4.5F - 0.05F;
    dim = std::clamp(1.0F - kMaxBackground / bright, dim, 0.85F);
  }
  if (m_wallpaperLease.texture().id == 0) {
    dim = 1.0F;
  }
  m_dim->setPosition(0.0F, 0.0F);
  m_dim->setSize(width, height);
  m_dim->setStyle(RoundedRectStyle{.fill = rgba(0.0F, 0.0F, 0.0F, dim), .fillMode = FillMode::Solid});

  const bool ryoku = m_draft.theme == 1;
  AuthColumn::State state;
  state.user = PamAuthenticator::currentUsername();
  state.placeholder = i18n::tr("lockscreen.password-placeholder");
  state.ownerMessage = config.lockscreen.ownerMessage;
  const auto [gradientFrom, gradientTo] =
      m_lockScreen != nullptr ? m_lockScreen->clockGradient() : std::pair<std::uint32_t, std::uint32_t>{0, 0};
  fillLockClockState(state, config.lockscreen, gradientFrom, gradientTo);
  AuthPlacementInput input{
      .layout = authLayoutFromName(LockEditorBar::kLayoutNames[static_cast<std::size_t>(m_draft.layout)]),
      .sw = width,
      .sh = height,
      .scale = scale,
      // Minimal hides its chrome until the pointer moves; the preview always shows it.
      .pointerMoved = true,
  };
  input.clockSize = clockSizeFor(config.lockscreen.clockSize);
  input.clockLines = std::max<int>(1, static_cast<int>(state.clockLines.size()));
  const AuthPlacement place = placeAuth(input);
  // Glance previews the glance layer: the auth sheet stays closed until a key, as on the lock.
  m_auth.setSheetOpen(false, true);
  m_auth.setState(std::move(state));
  m_auth.layout(renderer, place, width, scale);
  m_auth.glanceLayer().setVisible(!ryoku);

  m_themeNotice->setVisible(ryoku);
  if (ryoku) {
    m_themeNotice->setText(i18n::tr("lockscreen.editor.ryoku-preview"));
    m_themeNotice->setFontSize(15.0F * scale);
    m_themeNotice->setColor(rgba(1.0F, 1.0F, 1.0F, 0.96F));
    m_themeNotice->setMaxWidth(width - 64.0F * scale);
    m_themeNotice->measure(renderer);
    m_themeNotice->setPosition(
        std::round((width - m_themeNotice->width()) * 0.5F), std::round(height * 0.3F - m_themeNotice->height() * 0.5F)
    );
  }

  m_bar.setState(m_draft);
  m_bar.layout(renderer, width, scale);
}

void LockEditor::refresh() {
  if (m_surface != nullptr) {
    m_surface->requestLayout();
  }
}

void LockEditor::activate(LockEditorBar::Hit hit) {
  using Target = LockEditorBar::Target;
  using Segment = LockEditorBar::Segment;
  switch (hit.target) {
  case Target::Segment: {
    const auto segment = static_cast<Segment>(hit.index);
    if (segment == Segment::Layout || segment == Segment::Theme) {
      m_draft.panelOpen = !m_draft.panelOpen;
    } else if (LockEditorBar::segmentEnabled(hit.index)) {
      m_draft.mode = segment;
      m_draft.panelOpen = false;
    }
    break;
  }
  case Target::Done:
    save();
    close();
    return;
  case Target::LayoutCard:
    m_draft.layout = hit.index;
    break;
  case Target::ThemeChip:
    m_draft.theme = hit.index;
    break;
  case Target::None:
    m_draft.panelOpen = false;
    break;
  case Target::Panel:
  case Target::Bar:
    break;
  }
  refresh();
}

void LockEditor::moveFocus(int step) {
  const std::vector<LockEditorBar::Hit> order = LockEditorBar::focusOrder(m_draft.panelOpen);
  if (order.empty()) {
    return;
  }
  const auto it = std::ranges::find(order, m_draft.focus);
  const int count = static_cast<int>(order.size());
  int index = it == order.end() ? (step > 0 ? -1 : count) : static_cast<int>(it - order.begin());
  index = ((index + step) % count + count) % count;
  m_draft.focus = order[static_cast<std::size_t>(index)];
  refresh();
}

void LockEditor::save() {
  const std::string layout = LockEditorBar::kLayoutNames[static_cast<std::size_t>(m_draft.layout)];
  const std::string theme = LockEditorBar::kThemeNames[static_cast<std::size_t>(m_draft.theme)];
  const auto& lock = m_config->config().lockscreen;
  if (layout == lock.layout && theme == lock.theme) {
    return;
  }
  if (!m_config->setOverrides({{{"lockscreen", "layout"}, layout}, {{"lockscreen", "theme"}, theme}})) {
    kLog.warn("failed to save the lock screen layout {} and theme {}", layout, theme);
  }
}

bool LockEditor::onPointerEvent(const PointerEvent& event) {
  if (!isOpen()) {
    return false;
  }
  if (event.surface != nullptr && event.surface != m_surface->wlSurface()) {
    return false;
  }
  const auto x = static_cast<float>(event.sx);
  const auto y = static_cast<float>(event.sy);
  if (event.type == PointerEvent::Type::Motion || event.type == PointerEvent::Type::Enter) {
    const LockEditorBar::Hit hover = m_bar.hitTest(x, y);
    if (hover != m_draft.hover) {
      m_draft.hover = hover;
      refresh();
    }
  } else if (event.type == PointerEvent::Type::Leave) {
    m_draft.hover = {};
    refresh();
  } else if (event.type == PointerEvent::Type::Button && event.pressed && event.button == BTN_LEFT) {
    m_draft.focus = {};
    activate(m_bar.hitTest(x, y));
  }
  return true;
}

void LockEditor::onKeyboardEvent(const KeyboardEvent& event) {
  if (!isOpen() || !event.pressed || event.preedit) {
    return;
  }
  switch (event.sym) {
  case XKB_KEY_Escape:
    if (m_draft.panelOpen) {
      m_draft.panelOpen = false;
      m_draft.focus = {};
      refresh();
    } else {
      close();
    }
    return;
  case XKB_KEY_Tab:
    moveFocus((event.modifiers & KeyMod::Shift) != 0 ? -1 : 1);
    return;
  case XKB_KEY_ISO_Left_Tab:
  case XKB_KEY_Left:
  case XKB_KEY_Up:
    moveFocus(-1);
    return;
  case XKB_KEY_Right:
  case XKB_KEY_Down:
    moveFocus(1);
    return;
  case XKB_KEY_Return:
  case XKB_KEY_KP_Enter:
  case XKB_KEY_space:
    if (m_draft.focus.target != LockEditorBar::Target::None) {
      activate(m_draft.focus);
    }
    return;
  default:
    return;
  }
}
