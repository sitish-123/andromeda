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
  m_clock = {};
  const auto styles = clockStyles();
  for (std::size_t i = 0; i < styles.size(); ++i) {
    if (&styles[i] == &clockStyleById(lock.clockStyle)) {
      m_clock.style = static_cast<int>(i);
    }
  }
  m_clock.size = indexOf(LockEditorGallery::kSizeNames, lock.clockSize);
  m_clock.hour24 = lock.clockFormat24h;
  m_clock.seconds = lock.clockSeconds;

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
  const LockscreenConfig lock = previewConfig();
  const std::int64_t period = lock.clockSeconds || clockStyleById(lock.clockStyle).live ? 1000 : 60000;
  const auto now = std::chrono::system_clock::now();
  const auto into = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % period;
  m_clockTimer.start(std::chrono::milliseconds(period - into + 5), [this] {
    refresh();
    startClock();
  });
}

void LockEditor::close() {
  if (!isOpen()) {
    return;
  }
  m_clockTimer.stop();
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
  m_gallery.build(*m_root);
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
  const LockscreenConfig preview = previewConfig();
  fillLockClockState(state, preview, gradientFrom, gradientTo);
  AuthPlacementInput input{
      .layout = authLayoutFromName(LockEditorBar::kLayoutNames[static_cast<std::size_t>(m_draft.layout)]),
      .sw = width,
      .sh = height,
      .scale = scale,
      // Minimal hides its chrome until the pointer moves; the preview always shows it.
      .pointerMoved = true,
  };
  input.clockSize = clockSizeFor(preview.clockSize);
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

  // The clock slot: the date and clock block, 16 wider on each side (mock section 2: 400 x 128 at the 80 clock).
  const float top = std::min(place.date.y, place.clockY);
  const float bottom = std::max(place.date.bottom(), place.clockY + place.clockHeight());
  const float slotW = std::max(400.0F * scale, place.date.w + 32.0F * scale);
  const float slotX = place.clockCentred ? place.clockX - slotW * 0.5F : place.clockX - 16.0F * scale;
  m_clock.slotsMode = !ryoku && m_draft.mode == LockEditorBar::Segment::Slots && place.clock;
  m_gallery.layout(
      renderer, m_clock,
      {std::round(slotX), std::round(top - 12.0F * scale), std::round(slotW), std::round(bottom - top + 24.0F * scale)},
      width, height, scale
  );

  m_bar.setState(m_draft);
  m_bar.layout(renderer, width, scale);
}

LockscreenConfig LockEditor::previewConfig() const {
  LockscreenConfig lock = m_config->config().lockscreen;
  lock.clockStyle = std::string(clockStyles()[static_cast<std::size_t>(m_clock.style)].id);
  lock.clockSize = LockEditorGallery::kSizeNames[static_cast<std::size_t>(m_clock.size)];
  lock.clockFormat24h = m_clock.hour24;
  lock.clockSeconds = m_clock.seconds;
  return lock;
}

void LockEditor::setFocus(const Focus& focus) {
  m_draft.focus = {};
  m_clock.focus = {};
  if (const auto* bar = std::get_if<LockEditorBar::Hit>(&focus)) {
    m_draft.focus = *bar;
  } else {
    m_clock.focus = std::get<LockEditorGallery::Hit>(focus);
  }
}

void LockEditor::activateGallery(LockEditorGallery::Hit hit) {
  using Target = LockEditorGallery::Target;
  switch (hit.target) {
  case Target::ClockSlot:
    m_clock.open = !m_clock.open;
    m_draft.panelOpen = false;
    break;
  case Target::Style:
    m_clock.style = hit.index;
    startClock();
    break;
  case Target::Size:
    m_clock.size = hit.index;
    break;
  case Target::Hour24:
    m_clock.hour24 = !m_clock.hour24;
    break;
  case Target::Seconds:
    m_clock.seconds = !m_clock.seconds;
    startClock();
    break;
  case Target::None:
  case Target::Panel:
    break;
  }
  refresh();
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
      m_clock.open = false;
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
  std::vector<Focus> order;
  for (const auto& hit : LockEditorBar::focusOrder(m_draft.panelOpen)) {
    order.emplace_back(hit);
  }
  for (const auto& hit : LockEditorGallery::focusOrder(m_clock, static_cast<int>(clockStyles().size()))) {
    order.emplace_back(hit);
  }
  const Focus current = m_clock.focus.target != LockEditorGallery::Target::None ? Focus{m_clock.focus} : Focus{m_draft.focus};
  const auto it = std::ranges::find(order, current);
  const int count = static_cast<int>(order.size());
  int index = it == order.end() ? (step > 0 ? -1 : count) : static_cast<int>(it - order.begin());
  index = ((index + step) % count + count) % count;
  setFocus(order[static_cast<std::size_t>(index)]);
  refresh();
}

void LockEditor::save() {
  const std::string layout = LockEditorBar::kLayoutNames[static_cast<std::size_t>(m_draft.layout)];
  const std::string theme = LockEditorBar::kThemeNames[static_cast<std::size_t>(m_draft.theme)];
  const auto& lock = m_config->config().lockscreen;
  const LockscreenConfig preview = previewConfig();
  std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>> changes;
  const auto change = [&](const char* key, const auto& next, const auto& now) {
    if (next != now) {
      changes.push_back({{"lockscreen", key}, next});
    }
  };
  change("layout", layout, lock.layout);
  change("theme", theme, lock.theme);
  change("clock_style", preview.clockStyle, lock.clockStyle);
  change("clock_size", preview.clockSize, lock.clockSize);
  change("clock_format_24h", preview.clockFormat24h, lock.clockFormat24h);
  change("clock_seconds", preview.clockSeconds, lock.clockSeconds);
  if (!changes.empty() && !m_config->setOverrides(changes)) {
    kLog.warn("failed to save the lock screen editor draft");
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
  const LockEditorBar::Hit barHit = m_bar.hitTest(x, y);
  const LockEditorGallery::Hit galleryHit =
      barHit.target == LockEditorBar::Target::None ? m_gallery.hitTest(x, y) : LockEditorGallery::Hit{};
  if (event.type == PointerEvent::Type::Motion || event.type == PointerEvent::Type::Enter) {
    if (barHit != m_draft.hover || galleryHit != m_clock.hover) {
      m_draft.hover = barHit;
      m_clock.hover = galleryHit;
      refresh();
    }
  } else if (event.type == PointerEvent::Type::Leave) {
    m_draft.hover = {};
    m_clock.hover = {};
    refresh();
  } else if (event.type == PointerEvent::Type::Button && event.pressed && event.button == BTN_LEFT) {
    m_draft.focus = {};
    m_clock.focus = {};
    if (galleryHit.target != LockEditorGallery::Target::None) {
      activateGallery(galleryHit);
    } else {
      if (barHit.target == LockEditorBar::Target::None) {
        m_clock.open = false;
      }
      activate(barHit);
    }
  }
  return true;
}

void LockEditor::onKeyboardEvent(const KeyboardEvent& event) {
  if (!isOpen() || !event.pressed || event.preedit) {
    return;
  }
  switch (event.sym) {
  case XKB_KEY_Escape:
    if (m_draft.panelOpen || m_clock.open) {
      m_draft.panelOpen = false;
      m_clock.open = false;
      setFocus(LockEditorBar::Hit{});
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
    if (m_clock.focus.target != LockEditorGallery::Target::None) {
      activateGallery(m_clock.focus);
    } else if (m_draft.focus.target != LockEditorBar::Target::None) {
      activate(m_draft.focus);
    }
    return;
  default:
    return;
  }
}
