#include "shell/lockscreen/lock_editor.h"

#include "auth/pam_authenticator.h"
#include "config/config_service.h"
#include "core/deferred_call.h"
#include "core/log.h"
#include "core/ui_phase.h"
#include "i18n/i18n.h"
#include "shell/lockscreen/lock_thumbnail.h"
#include "shell/wallpaper/wallpaper_paths.h"
#include "render/core/render_styles.h"
#include "render/render_context.h"
#include "render/scene/node.h"
#include "render/scene/rect_node.h"
#include "render/scene/wallpaper_node.h"
#include "shell/desktop/desktop_widget_factory.h"
#include "shell/lockscreen/lock_screen.h"
#include "shell/lockscreen/lock_surface.h"
#include "shell/wallpaper/wallpaper_geometry.h"
#include "ui/auth/auth_layout.h"
#include "ui/auth/clock_style.h"
#include "ui/controls/label.h"
#include "wayland/wayland_connection.h"
#include "wayland/wayland_seat.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <format>
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
    LockScreen* lockScreen, const DesktopWidgetRuntimeServices& widgetServices
) {
  m_widgetFactory = std::make_unique<DesktopWidgetFactory>(widgetServices);
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
  m_widgets = LockCards::widgetList(lock.widgets);
  m_widgetsEdited = false;
  m_slotState = {};
  m_slotState.bottomSlot = indexOf(LockEditorSlots::kBottomNames, lock.bottomSlot);
  m_freeState = {};
  m_freeLoaded = false;
  m_freeEdited = false;
  m_freeDiscard = false;
  m_layoutPicked = false;
  m_pendingLayout = -1;
  m_wall = {};
  const auto own = lock.wallpaperOutputs.find(m_connector);
  m_wall.allDisplays = own == lock.wallpaperOutputs.end() || own->second.empty();
  m_wall.picked = m_wall.allDisplays                     ? lock.wallpaper
      : own->second == LockScreen::kDesktopWallpaper ? std::string{}
                                                     : own->second;
  m_wall.blur = std::clamp(lock.blurIntensity, 0.0F, 1.0F);
  m_wall.dim = std::clamp(lock.tintIntensity, 0.0F, LockEditorWallpaper::kMaxDim);
  m_wallItems.clear();
  m_wallDrag = LockEditorWallpaper::Target::None;
  m_wallPicked = false;
  m_blurEdited = false;
  m_dimEdited = false;
  m_wallScroll = 0;

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
  // The live widgets go first, while the scene that holds their nodes (and their GL context) is still alive.
  if (m_renderContext != nullptr) {
    m_renderContext->makeCurrent(m_surface->renderTarget());
  }
  m_free.clear();
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
    m_focusFrames = 120;
  }
  // Keys typed before the compositor hands the keyboard over are lost; tests wait for this line before typing.
  if (m_focusFrames > 0) {
    if (m_wayland->lastKeyboardSurface() == m_surface->wlSurface()) {
      m_focusFrames = -1;
      kLog.info("keyboard focus");
    } else {
      --m_focusFrames;
      m_surface->requestLayout();
    }
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
  m_cards.build(m_auth.glanceLayer());
  m_gallery.build(*m_root);
  m_wallpaperPanel.build(*m_root);
  m_wallpaperPanel.setThumbnailReadyCallback([this] { refresh(); });
  m_slots.build(*m_root);
  m_free.setFactory(m_widgetFactory.get(), &m_animations, [this] { refresh(); });
  m_free.build(*m_root);
  m_bar.build(*m_root);
  m_alert.build(*m_root);
  m_surface->setSceneRoot(m_root.get());
}

void LockEditor::applyWallpaper() {
  if (m_lockScreen == nullptr || m_textureCache == nullptr || m_wallpaper == nullptr) {
    return;
  }
  const auto& wallpaperConfig = m_config->config().wallpaper;
  m_wallpaper->setFillMode(wallpaperConfig.fillMode);
  std::string path = m_wall.picked;
  // "Same as desktop", a colour and a live wallpaper show the desktop image here (a colour fills the dim layer).
  if (path.empty() || path.starts_with("color:") || !LockScreen::liveWallpaperType(path).empty()) {
    path = wallpaperConfig.enabled ? m_config->getWallpaperPath(m_connector) : std::string{};
  }
  const bool own = !m_wall.picked.empty();
  if (path != m_wallpaperPath || own != m_ownWallpaper || m_wall.blur != m_wallpaperBlur) {
    m_wallpaperPath = path;
    m_ownWallpaper = own;
    m_wallpaperBlur = m_wall.blur;
    m_wallpaperLease.reset();
    if (!path.empty()) {
      // The preview has no compositor blur behind it, so every image blurs here as the lock's own image does.
      const float blur = m_wall.blur * kOwnImageBlur;
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

  float dim = m_wall.dim;
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
  Color colour;
  if (m_wall.picked.starts_with("color:") && tryParseHexColor(std::string_view(m_wall.picked).substr(6), colour)) {
    const float keep = 1.0F - m_wall.dim;
    m_dim->setStyle(RoundedRectStyle{
        .fill = rgba(colour.r * keep, colour.g * keep, colour.b * keep, 1.0F), .fillMode = FillMode::Solid
    });
  } else {
    m_dim->setStyle(RoundedRectStyle{.fill = rgba(0.0F, 0.0F, 0.0F, dim), .fillMode = FillMode::Solid});
  }

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
  const bool slotsMode = !ryoku && m_draft.mode == LockEditorBar::Segment::Slots;
  const bool rowLayout = input.layout == AuthLayout::Centered || input.layout == AuthLayout::Split;
  LockCards::Data cards = m_lockScreen != nullptr ? m_lockScreen->previewCards(preview) : LockCards::Data{};
  cards.preview = slotsMode;
  cards.rowSlots = slotsMode && rowLayout ? LockEditorSlots::kPlaces : 0;
  if (slotsMode) {
    // Gallery samples: one widget of each kind with this draft's live data.
    LockCards::Data samples = cards;
    samples.widgetIds.assign(LockCards::kWidgetKinds.begin(), LockCards::kWidgetKinds.end());
    samples.preview = true;
    LockCards sampler;
    sampler.setData(std::move(samples));
    m_slotState.samples = sampler.widgets();
  }
  m_cards.setData(std::move(cards));
  m_cards.fillPlacementInput(input);
  const AuthPlacement place = placeAuth(input);
  // Glance previews the glance layer: the auth sheet stays closed until a key, as on the lock.
  m_auth.setSheetOpen(false, true);
  m_auth.setState(std::move(state));
  m_auth.layout(renderer, place, width, scale);
  // Mock section 2: the account column sits at 55 % while editing; the editor never edits it.
  if (m_auth.columnLayer().visible() && m_auth.columnLayer().opacity() > 0.0F) {
    m_auth.columnLayer().setOpacity(0.55F);
  }
  m_cards.layout(renderer, place, scale);
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
  // Centered's date line spans the output: the slot hugs the clock instead (about 3 x the clock size wide).
  const float slotW = std::max(400.0F * scale, 3.0F * place.clockSize + 32.0F * scale);
  const float slotX = place.clockCentred ? place.clockX - slotW * 0.5F : place.clockX - 16.0F * scale;
  m_clock.slotsMode = !ryoku && m_draft.mode == LockEditorBar::Segment::Slots && place.clock;
  m_gallery.layout(
      renderer, m_clock,
      {std::round(slotX), std::round(top - 12.0F * scale), std::round(slotW), std::round(bottom - top + 24.0F * scale)},
      width, height, scale
  );
  m_wallpaperPanel.layout(renderer, m_wall, m_wallItems, width, height, scale);

  m_slotState.widgets = shownWidgets();
  m_slotState.active = slotsMode;
  m_slotState.places = LockCards::rowRect(place).empty() ? 0 : LockEditorSlots::kPlaces;
  m_slotState.bottom = place.layout == AuthLayout::Centered && !place.media.empty();
  std::vector<LockEditorSlots::Rect> placeRects;
  for (int i = 0; i < m_slotState.places; ++i) {
    const AuthRect r = LockCards::widgetRect(place, static_cast<std::size_t>(i), scale);
    placeRects.push_back({r.x, r.y, r.w, r.h});
  }
  m_slots.layout(
      renderer, m_slotState, placeRects, {place.media.x, place.media.y, place.media.w, place.media.h}, width, height,
      scale
  );

  m_width = width;
  m_height = height;
  m_scale = scale;
  const bool freeMode = !ryoku && m_draft.mode == LockEditorBar::Segment::Free;
  if (freeMode && !m_freeLoaded) {
    loadFreeItems(width, height);
  }
  m_zones = lock_free::zones(place, width, height, scale);
  m_freeState.active = freeMode;
  m_free.layout(renderer, m_freeState, m_zones, width, height, scale);

  m_draft.snapZones = m_freeState.snapZones;
  m_bar.setState(m_draft);
  m_bar.layout(renderer, width, scale);

  LockAlert::State alert;
  if (m_pendingLayout >= 0) {
    const std::string name = i18n::tr(
        std::string("lockscreen.editor.layouts.") + LockEditorBar::kLayoutNames[static_cast<std::size_t>(m_pendingLayout)]
    );
    alert.title = i18n::tr("lockscreen.editor.layout-alert-title", "name", name);
    alert.body = i18n::tr("lockscreen.editor.layout-alert-body");
    alert.confirm = i18n::tr("lockscreen.editor.layout-alert-confirm");
    alert.confirmFocused = m_alertConfirmFocus;
  }
  m_alert.setState(std::move(alert));
  m_alert.layout(renderer, width, height, scale);
}

LockscreenConfig LockEditor::previewConfig() const {
  LockscreenConfig lock = m_config->config().lockscreen;
  lock.clockStyle = std::string(clockStyles()[static_cast<std::size_t>(m_clock.style)].id);
  lock.clockSize = LockEditorGallery::kSizeNames[static_cast<std::size_t>(m_clock.size)];
  lock.clockFormat24h = m_clock.hour24;
  lock.clockSeconds = m_clock.seconds;
  lock.widgets = shownWidgets();
  lock.bottomSlot = LockEditorSlots::kBottomNames[static_cast<std::size_t>(m_slotState.bottomSlot)];
  return lock;
}

std::vector<std::string> LockEditor::shownWidgets() const {
  if (m_widgets.empty() && !m_widgetsEdited
      && authLayoutFromName(LockEditorBar::kLayoutNames[static_cast<std::size_t>(m_draft.layout)]) == AuthLayout::Split) {
    return {"phone", "notifications"};
  }
  return m_widgets;
}

void LockEditor::closePanels() {
  m_draft.panelOpen = false;
  m_wall.open = false;
  m_wallDrag = LockEditorWallpaper::Target::None;
  m_clock.open = false;
  m_slotState.panel = LockEditorSlots::Panel::None;
  m_slotState.editPlace = -1;
}

void LockEditor::activateSlots(LockEditorSlots::Hit hit, bool keyboard) {
  using Target = LockEditorSlots::Target;
  using Panel = LockEditorSlots::Panel;
  const auto backTo = [&](LockEditorSlots::Hit slot) {
    closePanels();
    if (keyboard) {
      setFocus(slot);
    }
  };
  switch (hit.target) {
  case Target::Place: {
    const int place = std::min(hit.index, static_cast<int>(shownWidgets().size()));
    const bool same = m_slotState.panel == Panel::Widget && m_slotState.editPlace == place;
    closePanels();
    if (!same) {
      m_slotState.panel = Panel::Widget;
      m_slotState.editPlace = place;
      if (keyboard) {
        setFocus(LockEditorSlots::Hit{Target::Kind, 0});
      }
    }
    break;
  }
  case Target::Kind: {
    std::vector<std::string> list = shownWidgets();
    const std::string kind(LockCards::kWidgetKinds[static_cast<std::size_t>(hit.index)]);
    const auto place = static_cast<std::size_t>(std::max(0, m_slotState.editPlace));
    if (place < list.size()) {
      list[place] = kind;
    } else {
      list.push_back(kind);
    }
    // A kind shows once: picking a placed kind moves it here.
    for (std::size_t i = 0; i < list.size(); ++i) {
      if (i != std::min(place, list.size() - 1) && list[i] == kind) {
        list.erase(list.begin() + static_cast<std::ptrdiff_t>(i));
        break;
      }
    }
    m_widgets = LockCards::widgetList(list);
    m_widgetsEdited = true;
    backTo({Target::Place, static_cast<int>(std::min(place, m_widgets.size() - 1))});
    break;
  }
  case Target::Remove: {
    std::vector<std::string> list = shownWidgets();
    if (m_slotState.editPlace >= 0 && m_slotState.editPlace < static_cast<int>(list.size())) {
      list.erase(list.begin() + m_slotState.editPlace);
    }
    m_widgets = std::move(list);
    m_widgetsEdited = true;
    backTo({Target::Place, std::max(0, m_slotState.editPlace)});
    break;
  }
  case Target::BottomSlot: {
    const bool open = m_slotState.panel == Panel::Bottom;
    closePanels();
    if (!open) {
      m_slotState.panel = Panel::Bottom;
      if (keyboard) {
        setFocus(LockEditorSlots::Hit{Target::BottomKind, m_slotState.bottomSlot});
      }
    }
    break;
  }
  case Target::BottomKind:
    m_slotState.bottomSlot = hit.index;
    backTo({Target::BottomSlot, -1});
    break;
  case Target::None:
  case Target::Panel:
    break;
  }
  refresh();
}

void LockEditor::loadFreeItems(float width, float height) {
  m_freeLoaded = true;
  m_freeState.items.clear();
  for (const DesktopWidgetState& widget : m_config->config().lockscreenWidgets.widgets) {
    if (widget.type == "login_box" || widget.outputName != m_connector) {
      continue;
    }
    const float sx = widget.placementWidth > 0.0F ? width / widget.placementWidth : 1.0F;
    const float sy = widget.placementHeight > 0.0F ? height / widget.placementHeight : 1.0F;
    // An auto-sized widget (box 0) shows as a 160 x 96 box until it is resized here.
    const float w = widget.boxWidth > 0.0F ? widget.boxWidth * sx : 160.0F * m_scale;
    const float h = widget.boxHeight > 0.0F ? widget.boxHeight * sy : 96.0F * m_scale;
    m_freeState.items.push_back(
        {widget.id, widget.type, {widget.cx * sx - w * 0.5F, widget.cy * sy - h * 0.5F, w, h}, widget.settings}
    );
  }
}

void LockEditor::activateFree(LockEditorFree::Hit hit) {
  using Target = LockEditorFree::Target;
  switch (hit.target) {
  case Target::Item:
  case Target::Resize:
    m_freeState.selected = hit.index;
    break;
  case Target::Add: {
    const float s = m_scale;
    const AuthRect box{0.0F, 0.0F, 160.0F * s, 96.0F * s};
    // The middle band first, then the top band, from the band's centre rightwards and then from its left edge, past
    // the boxes already there; a full output adds nothing.
    const auto free = [&](const AuthRect& r) {
      return std::ranges::none_of(m_freeState.items, [&](const LockEditorFree::Item& item) {
        return lock_free::overlaps(item.box, r);
      });
    };
    std::optional<AuthRect> placed;
    const bool snap = m_freeState.snapZones;
    const std::array bands{std::pair{m_zones.middle, lock_free::Zone::Middle}, std::pair{m_zones.top, lock_free::Zone::Top}};
    for (const auto& [band, zone] : bands) {
      // With Snap to zones a widget lives in one band: skip a band it does not fit.
      if (snap && (box.h > band.h || box.w > band.w)) {
        continue;
      }
      const float step = box.w + lock_free::kFreeGrid * s;
      const float centre = band.x + (band.w - box.w) * 0.5F;
      for (float x = centre; !placed.has_value() && x + box.w <= band.right(); x += step) {
        const auto at = lock_free::place(
            {x, band.y + (band.h - box.h) * 0.5F, box.w, box.h}, m_zones, snap, snap ? zone : lock_free::Zone::None,
            true, m_width, m_height, s
        );
        if (at.has_value() && free(*at)) {
          placed = at;
        }
      }
      for (float x = band.x; !placed.has_value() && x < centre; x += step) {
        const auto at = lock_free::place(
            {x, band.y + (band.h - box.h) * 0.5F, box.w, box.h}, m_zones, snap, snap ? zone : lock_free::Zone::None,
            true, m_width, m_height, s
        );
        if (at.has_value() && free(*at)) {
          placed = at;
        }
      }
      if (placed.has_value()) {
        break;
      }
    }
    if (!placed.has_value()) {
      break;
    }
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    m_freeState.items.push_back(
        {std::format("lockscreen-widget-{:016x}", static_cast<std::uint64_t>(stamp)),
         LockEditorFree::kTypes[static_cast<std::size_t>(hit.index)], *placed, {}}
    );
    m_freeState.selected = static_cast<int>(m_freeState.items.size()) - 1;
    m_freeEdited = true;
    break;
  }
  case Target::None:
    m_freeState.selected = -1;
    break;
  case Target::Toolbar:
    break;
  }
  refresh();
}

void LockEditor::nudgeFree(int dx, int dy, bool big, bool resize) {
  const int index = m_freeState.selected;
  if (index < 0 || index >= static_cast<int>(m_freeState.items.size())) {
    return;
  }
  AuthRect& box = m_freeState.items[static_cast<std::size_t>(index)].box;
  const auto next = lock_free::place(
      lock_free::nudge(box, dx, dy, big, resize, m_scale), m_zones, m_freeState.snapZones, lock_free::zoneOf(box, m_zones),
      false, m_width, m_height, m_scale
  );
  if (next.has_value()) {
    box = *next;
    m_freeEdited = true;
    refresh();
  }
}

// This output's lock widgets become the Free items; other outputs and the login box keep theirs.
void LockEditor::saveFree() {
  LockscreenWidgetsConfig widgets = m_config->config().lockscreenWidgets;
  std::vector<DesktopWidgetState> kept;
  std::vector<DesktopWidgetState> mine;
  for (DesktopWidgetState& widget : widgets.widgets) {
    (widget.type != "login_box" && widget.outputName == m_connector ? mine : kept).push_back(std::move(widget));
  }
  for (const LockEditorFree::Item& item : m_freeState.items) {
    DesktopWidgetState state;
    if (const auto old = std::ranges::find(mine, item.id, &DesktopWidgetState::id); old != mine.end()) {
      state = *old;
    }
    state.id = item.id;
    state.type = item.type;
    state.settings = item.settings;
    state.outputName = m_connector;
    state.cx = item.box.x + item.box.w * 0.5F;
    state.cy = item.box.y + item.box.h * 0.5F;
    state.placementWidth = m_width;
    state.placementHeight = m_height;
    state.boxWidth = item.box.w;
    state.boxHeight = item.box.h;
    kept.push_back(std::move(state));
  }
  widgets.widgets = std::move(kept);
  widgets.enabled = std::ranges::any_of(widgets.widgets, [](const DesktopWidgetState& w) { return w.type != "login_box"; });
  if (!m_config->setLockscreenWidgetsState(widgets)) {
    kLog.warn("failed to save the lock screen widgets");
  }
}

bool LockEditor::hasFreeWidgets() const {
  if (m_freeLoaded) {
    return !m_freeState.items.empty();
  }
  return std::ranges::any_of(m_config->config().lockscreenWidgets.widgets, [this](const DesktopWidgetState& widget) {
    return widget.type != "login_box" && widget.outputName == m_connector;
  });
}

void LockEditor::pickLayout(int index) {
  if (hasFreeWidgets()) {
    m_pendingLayout = index;
    m_alertConfirmFocus = false;
    return;
  }
  m_draft.layout = index;
  m_layoutPicked = true;
}

void LockEditor::closeAlert(bool confirm) {
  if (confirm && m_pendingLayout >= 0) {
    m_draft.layout = m_pendingLayout;
    m_layoutPicked = true;
    m_freeState.items.clear();
    m_freeState.selected = -1;
    m_freeLoaded = true;
    m_freeEdited = false;
    m_freeDiscard = true;
  }
  m_pendingLayout = -1;
  refresh();
}

void LockEditor::setFocus(const Focus& focus) {
  m_draft.focus = {};
  m_clock.focus = {};
  m_slotState.focus = {};
  m_freeState.focus = {};
  m_wall.focus = {};
  if (const auto* bar = std::get_if<LockEditorBar::Hit>(&focus)) {
    m_draft.focus = *bar;
  } else if (const auto* clock = std::get_if<LockEditorGallery::Hit>(&focus)) {
    m_clock.focus = *clock;
  } else if (const auto* slot = std::get_if<LockEditorSlots::Hit>(&focus)) {
    m_slotState.focus = *slot;
  } else if (const auto* wall = std::get_if<LockEditorWallpaper::Hit>(&focus)) {
    m_wall.focus = *wall;
    if (wall->target == LockEditorWallpaper::Target::Tile) {
      m_wall.scrollRow = LockEditorWallpaper::rowFor(wall->index, m_wall.scrollRow);
    }
  } else {
    m_freeState.focus = std::get<LockEditorFree::Hit>(focus);
  }
}

void LockEditor::loadWallpaperItems() {
  const ThemeMode configured = shellThemeMode(m_config->config().theme);
  const std::string directory = wallpaper::resolveGlobalWallpaperDirectory(
      m_config->config().wallpaper, wallpaper::effectiveThemeMode(configured, configured == ThemeMode::Light)
  );
  m_wallItems = LockEditorWallpaper::items(m_wall.tab, directory);
  for (auto& item : m_wallItems) {
    if (item.kind != LockEditorWallpaper::Item::Kind::Video && item.kind != LockEditorWallpaper::Item::Kind::Shader) {
      continue;
    }
    item.still = lock_thumbnail::find(item.value);
    if (item.still.empty()) {
      lock_thumbnail::request(item.value, [this, file = item.value] {
        // The editor may have closed or changed tabs since the request.
        for (auto& shown : m_wallItems) {
          if (shown.value == file) {
            shown.still = lock_thumbnail::find(file);
          }
        }
        refresh();
      });
    }
  }
  m_wall.emptyText = i18n::tr("lockscreen.editor.wallpaper-empty", "dir", directory);
  m_wall.scrollRow = 0;
}

void LockEditor::activateWallpaper(LockEditorWallpaper::Hit hit, float x, int step) {
  using Target = LockEditorWallpaper::Target;
  switch (hit.target) {
  case Target::Tab:
    m_wall.tab = hit.index;
    loadWallpaperItems();
    break;
  case Target::Scope:
    m_wall.allDisplays = !m_wall.allDisplays;
    m_wallPicked = true;
    break;
  case Target::Tile:
    if (hit.index >= 0 && hit.index < static_cast<int>(m_wallItems.size())) {
      m_wall.picked = m_wallItems[static_cast<std::size_t>(hit.index)].value;
      m_wallPicked = true;
    }
    break;
  case Target::Blur:
  case Target::Dim: {
    const bool blur = hit.target == Target::Blur;
    const float now = blur ? m_wall.blur : m_wall.dim / LockEditorWallpaper::kMaxDim;
    const float fraction = x >= 0.0F ? m_wallpaperPanel.sliderFraction(hit.target, x)
                                     : std::clamp(now + 0.05F * static_cast<float>(step), 0.0F, 1.0F);
    (blur ? m_wall.blur : m_wall.dim) = blur ? fraction : fraction * LockEditorWallpaper::kMaxDim;
    (blur ? m_blurEdited : m_dimEdited) = true;
    break;
  }
  case Target::None:
  case Target::Panel:
    break;
  }
  refresh();
}

void LockEditor::activateGallery(LockEditorGallery::Hit hit) {
  using Target = LockEditorGallery::Target;
  switch (hit.target) {
  case Target::ClockSlot: {
    const bool open = !m_clock.open;
    closePanels();
    m_clock.open = open;
    break;
  }
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
      const bool open = !m_draft.panelOpen;
      closePanels();
      m_draft.panelOpen = open;
    } else if (LockEditorBar::segmentEnabled(hit.index)) {
      m_draft.mode = segment;
      closePanels();
      if (segment == Segment::Wallpaper) {
        m_wall.open = true;
        loadWallpaperItems();
      }
    }
    break;
  }
  case Target::Snap:
    m_freeState.snapZones = !m_freeState.snapZones;
    break;
  case Target::Done:
    save();
    close();
    return;
  case Target::LayoutCard:
    pickLayout(hit.index);
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
  for (const auto& hit : LockEditorBar::focusOrder(m_draft.panelOpen, m_draft.mode == LockEditorBar::Segment::Free)) {
    order.emplace_back(hit);
  }
  for (const auto& hit : LockEditorGallery::focusOrder(m_clock, static_cast<int>(clockStyles().size()))) {
    order.emplace_back(hit);
  }
  for (const auto& hit : LockEditorSlots::focusOrder(m_slotState)) {
    order.emplace_back(hit);
  }
  for (const auto& hit : LockEditorFree::focusOrder(m_freeState)) {
    order.emplace_back(hit);
  }
  for (const auto& hit : LockEditorWallpaper::focusOrder(m_wall, static_cast<int>(m_wallItems.size()))) {
    order.emplace_back(hit);
  }
  const Focus current = m_wall.focus.target != LockEditorWallpaper::Target::None ? Focus{m_wall.focus}
      : m_clock.focus.target != LockEditorGallery::Target::None                  ? Focus{m_clock.focus}
      : m_slotState.focus.target != LockEditorSlots::Target::None             ? Focus{m_slotState.focus}
      : m_freeState.focus.target != LockEditorFree::Target::None              ? Focus{m_freeState.focus}
                                                                                : Focus{m_draft.focus};
  const auto it = std::ranges::find(order, current);
  const int count = static_cast<int>(order.size());
  int index = it == order.end() ? (step > 0 ? -1 : count) : static_cast<int>(it - order.begin());
  index = ((index + step) % count + count) % count;
  setFocus(order[static_cast<std::size_t>(index)]);
  refresh();
}

void LockEditor::save() {
  // Free placement is its own layout: the lock keeps the Centered auth area and draws the placed widgets.
  const std::string layout = m_freeEdited ? std::string("custom")
      : m_layoutPicked                     ? std::string(LockEditorBar::kLayoutNames[static_cast<std::size_t>(m_draft.layout)])
                                           : m_config->config().lockscreen.layout;
  if (m_freeEdited || m_freeDiscard) {
    saveFree();
  }
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
  if (m_widgetsEdited) {
    change("widgets", m_widgets, lock.widgets);
  }
  change("bottom_slot", preview.bottomSlot, lock.bottomSlot);
  if (m_blurEdited) {
    change("blur_intensity", static_cast<double>(m_wall.blur), static_cast<double>(lock.blurIntensity));
  }
  if (m_dimEdited) {
    change("tint_intensity", static_cast<double>(m_wall.dim), static_cast<double>(lock.tintIntensity));
  }
  if (m_wallPicked) {
    const auto own = lock.wallpaperOutputs.find(m_connector);
    const std::string ownNow = own != lock.wallpaperOutputs.end() ? own->second : std::string{};
    if (m_wall.allDisplays) {
      change("wallpaper", m_wall.picked, lock.wallpaper);
      // An empty entry falls back to lockscreen.wallpaper.
      if (!ownNow.empty()) {
        changes.push_back({{"lockscreen", "wallpaper_outputs", m_connector}, std::string{}});
      }
    } else {
      // "Same as desktop" on one display needs its own value: an empty entry would show lockscreen.wallpaper.
      const std::string next = m_wall.picked.empty() ? std::string(LockScreen::kDesktopWallpaper) : m_wall.picked;
      if (next != ownNow) {
        changes.push_back({{"lockscreen", "wallpaper_outputs", m_connector}, next});
      }
    }
  }
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
  if (m_pendingLayout >= 0) {
    if (event.type == PointerEvent::Type::Button && event.pressed && event.button == BTN_LEFT) {
      const LockAlert::Hit hit = m_alert.hitTest(x, y);
      if (hit == LockAlert::Hit::Cancel || hit == LockAlert::Hit::Confirm) {
        closeAlert(hit == LockAlert::Hit::Confirm);
      }
    }
    return true;
  }
  const LockEditorBar::Hit barHit = m_bar.hitTest(x, y);
  if (m_wallDrag != LockEditorWallpaper::Target::None) {
    if (event.type == PointerEvent::Type::Motion) {
      activateWallpaper({m_wallDrag, -1}, x);
    } else if (event.type == PointerEvent::Type::Button && !event.pressed && event.button == BTN_LEFT) {
      m_wallDrag = LockEditorWallpaper::Target::None;
    }
    return true;
  }
  const LockEditorWallpaper::Hit wallHit =
      barHit.target == LockEditorBar::Target::None ? m_wallpaperPanel.hitTest(x, y) : LockEditorWallpaper::Hit{};
  if (wallHit.target != LockEditorWallpaper::Target::None) {
    using WallTarget = LockEditorWallpaper::Target;
    if (event.type == PointerEvent::Type::Motion || event.type == PointerEvent::Type::Enter) {
      if (wallHit != m_wall.hover) {
        m_wall.hover = wallHit;
        refresh();
      }
    } else if (event.type == PointerEvent::Type::Axis && event.axis == 0) {
      // Vertical wheel: one row per notch.
      const int rows = (static_cast<int>(m_wallItems.size()) + LockEditorWallpaper::kColumns - 1)
          / LockEditorWallpaper::kColumns;
      const int last = std::max(0, rows - LockEditorWallpaper::kRows);
      // One row per wheel notch (120) or per 10 px of touchpad travel.
      m_wallScroll += event.axisValue120 != 0 ? event.axisValue120 : static_cast<int>(std::lround(event.axisValue * 12.0));
      const int steps = m_wallScroll / 120;
      m_wallScroll -= steps * 120;
      if (steps != 0) {
        m_wall.scrollRow = std::clamp(m_wall.scrollRow + steps, 0, last);
        refresh();
      }
    } else if (event.type == PointerEvent::Type::Button && event.pressed && event.button == BTN_LEFT) {
      setFocus(LockEditorBar::Hit{});
      if (wallHit.target == WallTarget::Blur || wallHit.target == WallTarget::Dim) {
        m_wallDrag = wallHit.target;
      }
      activateWallpaper(wallHit, x);
    }
    return true;
  }
  if (m_wall.hover.target != LockEditorWallpaper::Target::None) {
    m_wall.hover = {};
    refresh();
  }
  const LockEditorGallery::Hit galleryHit =
      barHit.target == LockEditorBar::Target::None ? m_gallery.hitTest(x, y) : LockEditorGallery::Hit{};
  const LockEditorSlots::Hit slotHit =
      barHit.target == LockEditorBar::Target::None && galleryHit.target == LockEditorGallery::Target::None
      ? m_slots.hitTest(x, y)
      : LockEditorSlots::Hit{};
  LockEditorFree::Hit freeHit =
      barHit.target == LockEditorBar::Target::None ? m_free.hitTest(x, y) : LockEditorFree::Hit{};
  // The boxes are drawn from the last frame: a box removed since then is gone.
  if (freeHit.index >= static_cast<int>(m_freeState.items.size())
      && (freeHit.target == LockEditorFree::Target::Item || freeHit.target == LockEditorFree::Target::Resize)) {
    freeHit = {};
  }
  if (m_free.dragging()) {
    auto& item = m_freeState.items[static_cast<std::size_t>(m_free.dragIndex())];
    if (event.type == PointerEvent::Type::Motion && m_free.moved(x, y)) {
      item.box = m_free.drag(x, y, m_scale);
      refresh();
    } else if (event.type == PointerEvent::Type::Button && !event.pressed && event.button == BTN_LEFT) {
      if (m_free.moved(x, y)) {
        // A refused drop (over the auth area) puts the box back where the drag started.
        item.box = m_free.end(x, y, m_freeState, m_zones, m_width, m_height, m_scale).value_or(m_freeDragStart);
        m_freeEdited = true;
      } else {
        // A click only selects.
        item.box = m_freeDragStart;
        m_free.cancel();
      }
      refresh();
    }
    return true;
  }
  if (event.type == PointerEvent::Type::Motion || event.type == PointerEvent::Type::Enter) {
    if (freeHit != m_freeState.hover) {
      m_freeState.hover = freeHit;
      refresh();
    }
    if (barHit != m_draft.hover || galleryHit != m_clock.hover || slotHit != m_slotState.hover) {
      m_draft.hover = barHit;
      m_clock.hover = galleryHit;
      m_slotState.hover = slotHit;
      refresh();
    }
  } else if (event.type == PointerEvent::Type::Leave) {
    m_draft.hover = {};
    m_clock.hover = {};
    m_slotState.hover = {};
    m_freeState.hover = {};
    refresh();
  } else if (event.type == PointerEvent::Type::Button && event.pressed && event.button == BTN_LEFT) {
    setFocus(LockEditorBar::Hit{});
    if (galleryHit.target != LockEditorGallery::Target::None) {
      activateGallery(galleryHit);
    } else if (slotHit.target != LockEditorSlots::Target::None) {
      activateSlots(slotHit, false);
    } else if (m_freeState.active && barHit.target == LockEditorBar::Target::None) {
      activateFree(freeHit);
      if (freeHit.target == LockEditorFree::Target::Item || freeHit.target == LockEditorFree::Target::Resize) {
        m_freeDragStart = m_freeState.items[static_cast<std::size_t>(freeHit.index)].box;
        m_free.begin(m_freeState, freeHit.index, freeHit.target == LockEditorFree::Target::Resize, x, y);
      }
    } else {
      if (barHit.target == LockEditorBar::Target::None) {
        closePanels();
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
  if (m_pendingLayout >= 0) {
    // Rulebook 9.10: Cancel has focus; Tab or arrows move it, Enter picks the focused button, Escape cancels.
    switch (event.sym) {
    case XKB_KEY_Escape:
      closeAlert(false);
      return;
    case XKB_KEY_Tab:
    case XKB_KEY_ISO_Left_Tab:
    case XKB_KEY_Left:
    case XKB_KEY_Right:
      m_alertConfirmFocus = !m_alertConfirmFocus;
      refresh();
      return;
    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter:
    case XKB_KEY_space:
      closeAlert(m_alertConfirmFocus);
      return;
    default:
      return;
    }
  }
  switch (event.sym) {
  case XKB_KEY_Escape:
    if (m_draft.panelOpen || m_clock.open || m_wall.open || m_slotState.panel != LockEditorSlots::Panel::None) {
      closePanels();
      setFocus(LockEditorBar::Hit{});
      refresh();
    } else {
      close();
    }
    return;
  case XKB_KEY_Tab:
    moveFocus((event.modifiers & KeyMod::Shift) != 0 ? -1 : 1);
    return;
  case XKB_KEY_Delete:
  case XKB_KEY_BackSpace:
    if (m_freeState.active && m_freeState.selected >= 0) {
      m_free.cancel();
      m_freeState.items.erase(m_freeState.items.begin() + m_freeState.selected);
      m_freeState.selected = -1;
      setFocus(LockEditorBar::Hit{});
      m_freeEdited = true;
      refresh();
    }
    return;
  default:
    break;
  }
  // Free with a selected widget: arrows move it 4 (Shift 32), Ctrl+arrows resize it.
  if (m_freeState.active && m_freeState.selected >= 0) {
    const bool big = (event.modifiers & KeyMod::Shift) != 0;
    const bool resize = (event.modifiers & KeyMod::Ctrl) != 0;
    switch (event.sym) {
    case XKB_KEY_Left:
      nudgeFree(-1, 0, big, resize);
      return;
    case XKB_KEY_Right:
      nudgeFree(1, 0, big, resize);
      return;
    case XKB_KEY_Up:
      nudgeFree(0, -1, big, resize);
      return;
    case XKB_KEY_Down:
      nudgeFree(0, 1, big, resize);
      return;
    default:
      break;
    }
  }
  if ((m_wall.focus.target == LockEditorWallpaper::Target::Blur || m_wall.focus.target == LockEditorWallpaper::Target::Dim)
      && (event.sym == XKB_KEY_Left || event.sym == XKB_KEY_Right)) {
    activateWallpaper(m_wall.focus, -1.0F, event.sym == XKB_KEY_Right ? 1 : -1);
    return;
  }
  switch (event.sym) {
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
    if (m_wall.focus.target != LockEditorWallpaper::Target::None) {
      activateWallpaper(m_wall.focus, -1.0F);
    } else if (m_clock.focus.target != LockEditorGallery::Target::None) {
      activateGallery(m_clock.focus);
    } else if (m_slotState.focus.target != LockEditorSlots::Target::None) {
      activateSlots(m_slotState.focus, true);
    } else if (m_freeState.focus.target != LockEditorFree::Target::None) {
      activateFree(m_freeState.focus);
    } else if (m_draft.focus.target != LockEditorBar::Target::None) {
      activate(m_draft.focus);
    }
    return;
  default:
    return;
  }
}
