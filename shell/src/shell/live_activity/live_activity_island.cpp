#include "shell/live_activity/live_activity_island.h"

#include "config/config_service.h"
#include "config/config_types.h"
#include "core/deferred_call.h"
#include "core/log.h"
#include "core/ui_phase.h"
#include "calendar/calendar_service.h"
#include "dbus/mpris/mpris_art.h"
#include "dbus/mpris/mpris_service.h"
#include "notification/notification_display_name.h"
#include "notification/notification_manager.h"
#include "notification/notification.h"
#include "pipewire/pipewire_spectrum.h"
#include "ui/controls/image.h"
#include "ui/surface_shadow.h"
#include "ui/controls/progress_bar.h"
#include "pipewire/pipewire_service.h"
#include "render/animation/motion_service.h"
#include "render/render_context.h"
#include "render/scene/node.h"
#include "ui/builders.h"
#include "render/scene/input_area.h"
#include "ui/controls/box.h"
#include "ui/controls/button.h"
#include "ui/controls/label.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "wayland/dsk_surface_client.h"
#include "wayland/surface.h"
#include "wayland/wayland_connection.h"
#include "wayland/wayland_seat.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <ctime>
#include <string>
#include <utility>

namespace {

  constexpr Logger kLog("live-activity");

  constexpr float kWidth = 440.0F;
  TOKENS_LIVE(kHeight, Tokens::componentLiveHeight);
  // Below this logical output width the island drops its progress bar, so it clears the bar's end island.
  constexpr std::int32_t kWideOutput = 1440;
  constexpr float kCompactWidth = 290.0F;
  // Narrowest live card.
  constexpr float kCollapsedWidth = 120.0F;
  TOKENS_LIVE(kPadding, Tokens::componentLivePadding);
  TOKENS_LIVE(kIcon, Tokens::componentLiveLead);
  TOKENS_LIVE(kButton, Tokens::componentLiveButton);
  TOKENS_LIVE(kProgressWidth, Tokens::componentLiveProgressWidth);
  TOKENS_LIVE(kProgressHeight, Tokens::componentLiveProgressHeight);
  // White on the dark live tint in both schemes (docs/design-system.md "Live activity").
  constexpr Color kWhite{.r = 1.0F, .g = 1.0F, .b = 1.0F, .a = 1.0F};
  Color white(float alpha) { return Color{.r = 1.0F, .g = 1.0F, .b = 1.0F, .a = alpha}; }

  Button::ButtonPalette livePalette(bool destructive) {
    const ColorSpec bg = destructive ? colorSpecFromRole(ColorRole::Danger) : fixedColorSpec(white(0.14F));
    const ColorSpec hover = destructive ? colorSpecFromRole(ColorRole::Danger, 0.85F) : fixedColorSpec(white(0.22F));
    const Button::ButtonStateColors normal{.bg = bg, .border = clearColorSpec(), .label = fixedColorSpec(kWhite)};
    return Button::ButtonPalette{
        .normal = normal,
        .hover = {.bg = hover, .border = clearColorSpec(), .label = fixedColorSpec(kWhite)},
        .pressed = {.bg = fixedColorSpec(white(0.3F)), .border = clearColorSpec(), .label = fixedColorSpec(kWhite)},
        .disabled = normal,
    };
  }
  // Width left for text: 440 - 2 x 8 padding - 36 icon - 150 bar - 36 button - 3 x 12 gaps.
  constexpr float kTitleMaxWidth = 166.0F;
  // The compositor clip uses the same preset (syncDsk), so the fill and the clip stay together.
  desk::motion::SpringConfig revealSpring(bool grow) {
    const Tokens::Spring& token = grow ? Tokens::springIslandExpand : Tokens::springIslandCollapse;
    return {.damping = token.damping, .stiffness = token.stiffness};
  }
  TOKENS_LIVE(kLiveRadius, Tokens::componentLiveRadius);
  TOKENS_LIVE(kDot, Tokens::componentIslandDot);
  TOKENS_LIVE(kRestWidth, Tokens::componentIslandCenterRestWidth);
  constexpr float kArt = 22.0F;
  constexpr float kArtRadius = 6.0F;
  constexpr float kRestPadding = 12.0F;
  constexpr std::int32_t kShadowPad = ui::surface_shadow::kBlurRadius + 2;
  constexpr float kBannerHeight = 76.0F;
  constexpr float kBannerRadius = 22.0F;
  constexpr float kOsdWidth = 262.0F;
  constexpr float kOsdBarWidth = 150.0F;
  constexpr float kOsdBarHeight = 6.0F;
  constexpr float kHoverArt = 40.0F;
  constexpr float kHoverButton = 30.0F;
  constexpr float kHoverGap = 10.0F;
  constexpr float kHoverPad = 8.0F;
  // Art, three buttons, four gaps and the padding: what the media face needs besides its text.
  constexpr float kHoverFixed = 2.0F * kHoverPad + kHoverArt + 3.0F * kHoverButton + 4.0F * kHoverGap;
  constexpr float kHoverTextMax = 190.0F;
  constexpr float kIdleTextMax = 220.0F;
  constexpr float kBannerGlyph = 36.0F;
  constexpr float kBarGap = 2.0F;
  constexpr float kBarWidth = 3.0F;
  constexpr auto kCollapseDelay = std::chrono::milliseconds(300);
  constexpr auto kOsdTime = std::chrono::milliseconds(1500);
  constexpr auto kBannerTime = std::chrono::milliseconds(5000);

  Button::ButtonPalette ghostPalette() {
    const Button::ButtonStateColors normal{.bg = clearColorSpec(), .border = clearColorSpec(), .label = fixedColorSpec(kWhite)};
    return Button::ButtonPalette{
        .normal = normal,
        .hover = {.bg = fixedColorSpec(white(0.16F)), .border = clearColorSpec(), .label = fixedColorSpec(kWhite)},
        .pressed = {.bg = fixedColorSpec(white(0.3F)), .border = clearColorSpec(), .label = fixedColorSpec(kWhite)},
        .disabled = normal,
    };
  }

  std::string timeText() {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_r(&now, &local);
    std::array<char, 16> buffer{};
    const std::size_t n = std::strftime(buffer.data(), buffer.size(), "%H:%M", &local);
    return {buffer.data(), n};
  }

  std::string fullDateText() {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_r(&now, &local);
    std::array<char, 64> buffer{};
    const std::size_t n = std::strftime(buffer.data(), buffer.size(), "%A, %-d %B", &local);
    return {buffer.data(), n};
  }

  bool hasDefaultAction(const std::vector<std::string>& actions) {
    for (std::size_t i = 0; i < actions.size(); i += 2) {
      if (actions[i] == "default") {
        return true;
      }
    }
    return false;
  }

  std::string dateText() {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_r(&now, &local);
    std::array<char, 32> buffer{};
    const std::size_t n = std::strftime(buffer.data(), buffer.size(), "%a %-d %b", &local);
    return {buffer.data(), n};
  }

  float lerp(float a, float b, float t) { return a + (b - a) * t; }

} // namespace

LiveActivityIsland::LiveActivityIsland() = default;

LiveActivityIsland::~LiveActivityIsland() {
  m_alive.reset();
  destroySurfaces();
}

void LiveActivityIsland::initialize(WaylandConnection& wayland, ConfigService* config, RenderContext* renderContext) {
  m_wayland = &wayland;
  m_config = config;
  m_renderContext = renderContext;
}

void LiveActivityIsland::setVisibilityCallback(VisibilityCallback callback) {
  m_visibilityCallback = std::move(callback);
}

void LiveActivityIsland::setSpaceProvider(SpaceProvider provider) { m_spaceProvider = std::move(provider); }

void LiveActivityIsland::setRestSources(
    std::function<PipeWireService*()> pipewire, std::function<MprisService*()> mpris, NotificationManager* notifications,
    std::function<void(wl_output*)> open
) {
  m_pipewire = std::move(pipewire);
  m_mpris = std::move(mpris);
  m_notifications = notifications;
  m_open = std::move(open);
}

void LiveActivityIsland::setActivity(std::optional<LiveActivity> activity) {
  const bool had = m_activity.has_value();
  m_activity = std::move(activity);
  if (m_activity.has_value()) {
    m_shown = *m_activity;
    // The banner waits behind the call, recording or transfer.
    m_bannerTimer.stop();
  } else if (had && m_banner.has_value() && !m_osd.has_value()) {
    m_bannerTimer.start(kBannerTime, [this, alive = std::weak_ptr<bool>(m_alive)] {
      if (!alive.expired()) {
        endBanner();
      }
    });
  } else if (had && !m_banner.has_value() && !m_osd.has_value()) {
    startNextBanner();
  }
  updateAllFaces();
  for (auto& inst : m_instances) {
    inst->surface->requestLayout();
  }
}

void LiveActivityIsland::setExtraSources(
    HttpClient* http, std::function<PipeWireSpectrum*()> spectrum, CalendarService* calendar
) {
  m_http = http;
  m_spectrum = std::move(spectrum);
  m_calendar = calendar;
}

void LiveActivityIsland::onTimeTick() {
  if (const std::string minute = timeText(); m_date != dateText() || m_minute != minute) {
    m_minute = minute;
    refresh();
  }
}

bool LiveActivityIsland::osdVisible() const { return m_osd.has_value(); }

bool LiveActivityIsland::coversAllOutputs() const {
  if (m_wayland == nullptr || m_instances.empty()) {
    return false;
  }
  return std::ranges::all_of(m_wayland->outputs(), [this](const auto& output) {
    if (!output.done || output.output == nullptr || !output.hasUsableGeometry()) {
      return true;
    }
    return std::ranges::any_of(m_instances, [&output](const auto& inst) { return inst->output == output.output; });
  });
}

bool LiveActivityIsland::showOsd(const OsdContent& content) {
  if (!coversAllOutputs() || m_activity.has_value()) {
    return false;
  }
  if (m_banner.has_value()) {
    m_bannerQueue.insert(m_bannerQueue.begin(), *m_banner);
    m_banner.reset();
    m_bannerTimer.stop();
  }
  m_osd = content;
  m_osdTimer.start(kOsdTime, [this, alive = std::weak_ptr<bool>(m_alive)] {
    if (alive.expired()) {
      return;
    }
    m_osd.reset();
    if (!m_bannerQueue.empty()) {
      startNextBanner();
    } else {
      updateAllFaces();
    }
    for (auto& inst : m_instances) {
      inst->surface->requestLayout();
    }
  });
  updateAllFaces();
  for (auto& inst : m_instances) {
    inst->surface->requestLayout();
  }
  return true;
}

bool LiveActivityIsland::showBanner(const Notification& n, NotificationEvent event) {
  const auto relayout = [this] {
    for (auto& inst : m_instances) {
      inst->surface->requestLayout();
    }
  };
  const auto item = [&n] {
    return BannerItem{
        .id = n.id,
        .title = n.summary.empty() ? notificationDisplayAppName(n) : n.summary,
        .body = n.body,
        .defaultAction = hasDefaultAction(n.actions),
    };
  };
  if (event == NotificationEvent::Closed) {
    m_consumed.erase(n.id);
    std::erase_if(m_bannerQueue, [&n](const BannerItem& b) { return b.id == n.id; });
    if (m_banner.has_value() && m_banner->id == n.id) {
      endBanner();
    }
    return false;
  }
  if (event == NotificationEvent::Updated) {
    if (m_banner.has_value() && m_banner->id == n.id) {
      m_banner = item();
      relayout();
      return true;
    }
    for (auto& queued : m_bannerQueue) {
      if (queued.id == n.id) {
        queued = item();
        return true;
      }
    }
    // The banner already ended: the update must not raise a toast for a notification the island showed.
    return m_consumed.contains(n.id);
  }
  if (!coversAllOutputs() || m_activity.has_value() || n.urgency == Urgency::Critical || n.timeout == 0) {
    return false;
  }
  m_consumed.insert(n.id);
  m_bannerQueue.push_back(item());
  if (!m_osd.has_value() && !m_banner.has_value()) {
    startNextBanner();
  }
  return true;
}

void LiveActivityIsland::startNextBanner() {
  if (m_bannerQueue.empty()) {
    updateAllFaces();
    return;
  }
  m_banner = std::move(m_bannerQueue.front());
  m_bannerQueue.erase(m_bannerQueue.begin());
  m_bannerTimer.start(kBannerTime, [this, alive = std::weak_ptr<bool>(m_alive)] {
    if (!alive.expired()) {
      endBanner();
    }
  });
  updateAllFaces();
  for (auto& inst : m_instances) {
    inst->surface->requestLayout();
  }
}

void LiveActivityIsland::endBanner() {
  m_banner.reset();
  m_bannerTimer.stop();
  if (!m_osd.has_value()) {
    startNextBanner();
  }
}

void LiveActivityIsland::refresh() {
  if (m_refreshScheduled) {
    return;
  }
  m_refreshScheduled = true;
  DeferredCall::callLater([this, alive = std::weak_ptr<bool>(m_alive)] {
    if (alive.expired()) {
      return;
    }
    m_refreshScheduled = false;
    refreshNow();
  });
}

std::function<void()> LiveActivityIsland::refresher() {
  return [this, alive = std::weak_ptr<bool>(m_alive)] {
    if (!alive.expired()) {
      refresh();
    }
  };
}

void LiveActivityIsland::refreshNow() {
  ensureSurfaces();
  if (!coversAllOutputs() && (m_osd.has_value() || m_banner.has_value() || !m_bannerQueue.empty())) {
    // An output without an island cannot show them; they stay in the notification history.
    m_osd.reset();
    m_banner.reset();
    m_bannerQueue.clear();
    m_osdTimer.stop();
    m_bannerTimer.stop();
  }
  updateAllFaces();
  syncBars();
  for (auto& inst : m_instances) {
    inst->surface->requestLayout();
  }
}

void LiveActivityIsland::onOutputChange() { refreshNow(); }

void LiveActivityIsland::ensureSurfaces() {
  if (m_wayland == nullptr || m_renderContext == nullptr || !m_spaceProvider) {
    return;
  }
  std::erase_if(m_instances, [this](const auto& inst) {
    const auto space = m_spaceProvider(inst->output);
    const bool outputGone = std::ranges::none_of(m_wayland->outputs(), [&inst](const auto& output) {
      return output.output == inst->output;
    });
    if (outputGone || !space.has_value() || space->bottom != inst->space.bottom) {
      inst->animations.cancelAll();
      return true;
    }
    // The live card width depends on the logical output width, which a scale change moves across kWideOutput.
    const auto output = std::ranges::find_if(m_wayland->outputs(), [&inst](const auto& o) { return o.output == inst->output; });
    const float width = output->logicalWidth >= kWideOutput ? kWidth : kCompactWidth;
    if (width != inst->width) {
      kLog.info("center island surface width {} -> {} (logical output width {})", inst->width, width, output->logicalWidth);
      inst->width = width;
      inst->surface->setAnchorAndSize(
          space->bottom ? LayerShellAnchor::Bottom : LayerShellAnchor::Top, static_cast<std::uint32_t>(width),
          static_cast<std::uint32_t>(surfaceHeight())
      );
      inst->dskTarget.reset();
    }
    if (space->edgeMargin != inst->space.edgeMargin) {
      inst->surface->setMargins(space->bottom ? 0 : surfaceMargin(*space), 0, space->bottom ? surfaceMargin(*space) : 0, 0);
      inst->pad = static_cast<float>(std::min(space->edgeMargin, kShadowPad));
      inst->dskTarget.reset();
    }
    inst->space = *space;
    return false;
  });

  for (const auto& output : m_wayland->outputs()) {
    if (!output.done || output.output == nullptr || !output.hasUsableGeometry()) {
      continue;
    }
    if (std::ranges::any_of(m_instances, [&output](const auto& inst) { return inst->output == output.output; })) {
      continue;
    }
    const auto space = m_spaceProvider(output.output);
    if (!space.has_value()) {
      continue;
    }

    auto inst = std::make_unique<Instance>();
    inst->output = output.output;
    inst->space = *space;
    inst->width = output.logicalWidth >= kWideOutput ? kWidth : kCompactWidth;
    kLog.info("center island surface width {} (logical output width {})", inst->width, output.logicalWidth);
    inst->surface = std::make_unique<LayerSurface>(
        *m_wayland,
        LayerSurfaceConfig{
            .nameSpace = "noctalia-live-activity",
            .layer = LayerShellLayer::Overlay,
            .anchor = space->bottom ? LayerShellAnchor::Bottom : LayerShellAnchor::Top,
            .width = static_cast<std::uint32_t>(inst->width),
            .height = static_cast<std::uint32_t>(surfaceHeight()),
            // -1: placed from the screen edge, over the bar's exclusive zone.
            .exclusiveZone = -1,
            .marginTop = space->bottom ? 0 : surfaceMargin(*space),
            .marginBottom = space->bottom ? surfaceMargin(*space) : 0,
            .keyboard = LayerShellKeyboard::None,
            .defaultWidth = static_cast<std::uint32_t>(inst->width),
            .defaultHeight = static_cast<std::uint32_t>(surfaceHeight()),
            .prewarmBlur = true,
        }
    );
    inst->surface->setRenderContext(m_renderContext);
    auto* instPtr = inst.get();
    inst->surface->setConfigureCallback([instPtr](std::uint32_t, std::uint32_t) { instPtr->surface->requestLayout(); });
    inst->surface->setPrepareFrameCallback([this, instPtr](bool needsUpdate, bool needsLayout) {
      prepareFrame(*instPtr, needsUpdate, needsLayout);
    });
    inst->surface->setAnimationManager(&inst->animations);
    if (!inst->surface->initialize(output.output)) {
      kLog.warn("failed to create the center island surface on {}", output.connectorName);
      continue;
    }
    inst->surface->setInputRegion({});
    if (auto* manager = m_wayland->dskSurfaceManager(); manager != nullptr) {
      inst->dsk = std::make_unique<DskSurfaceClient>(manager, inst->surface->wlSurface());
    }
    inst->liveWidth = std::min(inst->width, std::max(kCollapsedWidth, space->width));
    inst->restWidth = std::min(space->width, static_cast<float>(kRestWidth));
    inst->pad = static_cast<float>(std::min(space->edgeMargin, kShadowPad));
    inst->from = inst->to = inst->cur = faceRect(*inst, Face::Rest);
    syncDsk(*inst, false);
    m_instances.push_back(std::move(inst));
    updateFace(*m_instances.back());
  }
  syncVisibility();
}

void LiveActivityIsland::destroySurfaces() {
  for (auto& inst : m_instances) {
    inst->animations.cancelAll();
  }
  m_instances.clear();
  syncBars();
}


float LiveActivityIsland::surfaceHeight() const {
  return std::max(static_cast<float>(kHeight), kBannerHeight) + static_cast<float>(kShadowPad);
}

// The layer margin leaves room for the shadow bleed inside the surface, so the island keeps its screen position.
std::int32_t LiveActivityIsland::surfaceMargin(const CenterIslandSpace& space) {
  return space.edgeMargin - std::min(space.edgeMargin, kShadowPad);
}

LiveActivityIsland::Rect LiveActivityIsland::faceRect(const Instance& inst, Face face) const {
  float w = inst.liveWidth;
  float h = kHeight;
  float radius = kLiveRadius;
  switch (face) {
  case Face::Rest:
    h = std::min(inst.space.height, static_cast<float>(kHeight));
    w = inst.restWidth;
    radius = h * 0.5F;
    break;
  case Face::HoverMedia:
  case Face::HoverIdle:
  case Face::Osd:
    w = std::max(inst.restWidth, std::min(inst.faceWidth[static_cast<std::size_t>(face)], inst.liveWidth));
    break;
  case Face::Banner:
    h = kBannerHeight;
    radius = kBannerRadius;
    break;
  case Face::Live:
    break;
  }
  return Rect{
      .x = std::round((inst.width - w) * 0.5F),
      .y = inst.space.bottom ? surfaceHeight() - inst.pad - h : inst.pad,
      .w = w,
      .h = h,
      .radius = radius,
  };
}

LiveActivityIsland::Face LiveActivityIsland::desiredFace(const Instance& inst) const {
  if (m_activity.has_value()) {
    return Face::Live;
  }
  if (m_osd.has_value()) {
    return Face::Osd;
  }
  if (m_banner.has_value()) {
    return Face::Banner;
  }
  if (inst.hovered) {
    return inst.hoverMedia ? Face::HoverMedia : Face::HoverIdle;
  }
  return Face::Rest;
}

void LiveActivityIsland::updateAllFaces() {
  for (auto& inst : m_instances) {
    updateFace(*inst);
  }
}

void LiveActivityIsland::updateFace(Instance& inst) {
  const MprisService* mpris = m_mpris ? m_mpris() : nullptr;
  if (inst.hoverMedia && (mpris == nullptr || !mpris->activePlayer().has_value())) {
    inst.hoverMedia = false;
  }
  const Face face = desiredFace(inst);
  if (face == inst.face) {
    return;
  }
  // An interrupted morph keeps its speed: project the old px/s onto the new span.
  float velocity = 0.0F;
  if (inst.revealAnimId != 0) {
    const float v = inst.animations.velocity(inst.revealAnimId);
    const Rect nt = faceRect(inst, face);
    const float os[4] = {inst.to.x - inst.from.x, inst.to.y - inst.from.y, inst.to.w - inst.from.w, inst.to.h - inst.from.h};
    const float ns[4] = {nt.x - inst.cur.x, nt.y - inst.cur.y, nt.w - inst.cur.w, nt.h - inst.cur.h};
    float dot = 0.0F;
    float norm = 0.0F;
    for (int i = 0; i < 4; ++i) {
      dot += v * os[i] * ns[i];
      norm += ns[i] * ns[i];
    }
    velocity = norm > 1.0F ? dot / norm : 0.0F;
  }
  inst.from = inst.cur;
  inst.prevFace = inst.face;
  inst.face = face;
  inst.to = faceRect(inst, face);
  animateReveal(inst, velocity);
  inst.surface->requestLayout();
}

void LiveActivityIsland::hoverEnter(Instance& inst) {
  inst.collapse.stop();
  if (inst.hovered || m_activity.has_value()) {
    return;
  }
  const MprisService* mpris = m_mpris ? m_mpris() : nullptr;
  const auto player = mpris != nullptr ? mpris->activePlayer() : std::nullopt;
  inst.hoverMedia = player.has_value() && player->playbackStatus == "Playing";
  inst.hovered = true;
  updateFace(inst);
}

void LiveActivityIsland::hoverLeave(Instance& inst) {
  if (!inst.hovered) {
    return;
  }
  inst.collapse.start(kCollapseDelay, [this, &inst] {
    inst.hovered = false;
    updateFace(inst);
  });
}

void LiveActivityIsland::syncDsk(Instance& inst, bool animate) {
  if (inst.dsk == nullptr) {
    return;
  }
  const bool expanded = inst.face != Face::Rest;
  const Rect r = faceRect(inst, inst.face);
  if (inst.dskTarget.has_value() && inst.dskTarget->x == r.x && inst.dskTarget->y == r.y && inst.dskTarget->w == r.w
      && inst.dskTarget->h == r.h) {
    return;
  }
  const DskSurfaceClient::Value clip{r.x, r.y, r.w, r.h};
  const DskSurfaceClient::Value radius{r.radius, 0.0, 0.0, 0.0};
  if (!inst.dskTarget.has_value() || !animate || !MotionService::instance().enabled()) {
    inst.dsk->set(DskSurfaceClient::Property::Clip, clip);
    inst.dsk->set(DskSurfaceClient::Property::Radius, radius);
  } else {
    const auto curve = expanded ? DskSurfaceClient::Curve::SpringIslandExpand : DskSurfaceClient::Curve::SpringIslandCollapse;
    inst.dsk->animate(DskSurfaceClient::Property::Clip, curve, clip);
    inst.dsk->animate(DskSurfaceClient::Property::Radius, curve, radius);
  }
  if (expanded) {
    inst.dsk->setElevation(DskSurfaceClient::Elevation::E2);
  } else if (!animate || !MotionService::instance().enabled()) {
    inst.dsk->setElevation(DskSurfaceClient::Elevation::None);
  }
  inst.dskTarget = r;
  inst.surface->requestRedraw();
}

void LiveActivityIsland::prepareFrame(Instance& inst, bool needsUpdate, bool needsLayout) {
  const auto width = static_cast<float>(inst.surface->width());
  const auto height = static_cast<float>(inst.surface->height());
  if (width <= 0.0F || height <= 0.0F) {
    return;
  }
  m_renderContext->makeCurrent(inst.surface->renderTarget());
  UiPhaseScope layoutPhase(UiPhase::Layout);
  const bool build = inst.sceneRoot == nullptr || std::round(inst.sceneRoot->width()) != width
      || std::round(inst.sceneRoot->height()) != height;
  if (build) {
    buildScene(inst, width, height);
  }
  if (build || needsUpdate || needsLayout) {
    applyContent(inst);
    applyReveal(inst, inst.reveal);
  }
}

void LiveActivityIsland::buildScene(Instance& inst, float width, float height) {
  inst.sceneRoot = ui::node({});
  inst.sceneRoot->setSize(width, height);
  inst.surface->setSceneRoot(inst.sceneRoot.get());
  auto* instPtr = &inst;

  auto shadowBox = ui::box({.out = &inst.shadow});
  shadowBox->setStyle(ui::surface_shadow::style(
      m_config->config().shell.shadow, Style::glassAlpha(inst.space.backgroundOpacity), ui::surface_shadow::Shape{}
  ));
  auto material = ui::box({.out = &inst.material});
  material->setClipChildren(true);
  auto tint = ui::box({
      .out = &inst.tint,
      .fill = fixedColorSpec(rgbaHex(Tokens::componentLiveTint)),
  });
  tint->setClipChildren(true);

  const auto dot = [&](Box** out, ColorSpec fill) {
    return ui::box({
        .out = out,
        .fill = std::move(fill),
        .radius = kDot * 0.5F,
        .width = kDot,
        .height = kDot,
        .visible = false,
    });
  };
  const auto artPlaceholder = [](float size, float radius, float glyphSize) {
    return ui::row(
        {.align = FlexAlign::Center,
         .justify = FlexJustify::Center,
         .fill = colorSpecFromRole(ColorRole::Label3, 0.25F),
         .radius = radius,
         .width = size,
         .height = size},
        ui::glyph({.glyph = "music", .glyphSize = glyphSize, .color = colorSpecFromRole(ColorRole::Label2)})
    );
  };

  auto bars = ui::node({.width = 3.0F * kBarWidth + 2.0F * kBarGap, .height = kArt});
  inst.bars = bars.get();
  for (std::size_t i = 0; i < inst.barBox.size(); ++i) {
    auto bar = ui::box({.out = &inst.barBox[i], .fill = colorSpecFromRole(ColorRole::Accent), .radius = 1.5F});
    bar->setSize(kBarWidth, 6.0F);
    bar->setPosition(static_cast<float>(i) * (kBarWidth + kBarGap), (kArt - 6.0F) * 0.5F);
    bars->addChild(std::move(bar));
  }
  auto restArt = artPlaceholder(kArt, kArtRadius, 14.0F);
  inst.artPlaceholder = restArt.get();
  auto mediaGroup = ui::row(
      {.align = FlexAlign::Center, .gap = Style::spaceSm, .visible = false},
      std::move(restArt),
      ui::image({.out = &inst.art, .fit = ImageFit::Cover, .radius = kArtRadius, .width = kArt, .height = kArt, .visible = false}),
      std::move(bars)
  );
  inst.mediaGroup = mediaGroup.get();
  const float restFont = Tokens::typeBody.size;
  const auto restWeight = FontWeight::SemiBold;
  auto restContent = ui::row(
      {.align = FlexAlign::Center, .gap = Style::spaceSm},
      std::move(mediaGroup),
      ui::label({
          .out = &inst.date,
          .fontSize = restFont,
          .fontWeight = restWeight,
          .color = colorSpecFromRole(ColorRole::Label1),
          .maxLines = 1,
      }),
      ui::box({
          .out = &inst.divider,
          .fill = colorSpecFromRole(ColorRole::Label3),
          .width = 1.0F,
          .height = 14.0F,
          .visible = false,
      }),
      dot(&inst.micDot, colorSpecFromRole(ColorRole::Accent)),
      dot(&inst.cameraDot, colorSpecFromRole(ColorRole::Accent)),
      dot(&inst.screenDot, colorSpecFromRole(ColorRole::Danger)),
      ui::label({
          .out = &inst.count,
          .fontSize = restFont,
          .fontWeight = restWeight,
          .color = colorSpecFromRole(ColorRole::Label1),
          .maxLines = 1,
          .visible = false,
      })
  );
  inst.count->setLineHeight(restFont * Tokens::typeBody.lineHeight);
  inst.date->setLineHeight(restFont * Tokens::typeBody.lineHeight);
  inst.faceNode[static_cast<std::size_t>(Face::Rest)] = restContent.get();
  material->addChild(std::move(restContent));

  // Live card (call, recording, transfer).
  auto title = ui::label({
      .out = &inst.title,
      .fontSize = Tokens::typeTitle.size,
      .fontWeight = static_cast<FontWeight>(Tokens::typeTitle.weight),
      .color = fixedColorSpec(kWhite),
      .maxWidth = kTitleMaxWidth,
      .maxLines = 1,
  });
  auto subtitle = ui::label({
      .out = &inst.subtitle,
      .fontSize = Tokens::typeCaption.size,
      .color = fixedColorSpec(white(0.7F)),
      .maxWidth = kTitleMaxWidth,
      .maxLines = 1,
  });

  const auto iconCircle = [](Glyph** out, float size) {
    return ui::row(
        {
            .align = FlexAlign::Center,
            .justify = FlexJustify::Center,
            .fill = colorSpecFromRole(ColorRole::Accent),
            .radius = size * 0.5F,
            .width = size,
            .height = size,
        },
        ui::glyph({
            .out = out,
            .glyphSize = Tokens::componentIslandIcon,
            .color = colorSpecFromRole(ColorRole::OnAccent),
        })
    );
  };

  auto content = ui::row(
      {
          .align = FlexAlign::Center,
          .gap = Tokens::componentLiveGap,
          .paddingH = kPadding,
          .width = inst.width,
          .height = kHeight,
      },
      iconCircle(&inst.icon, kIcon),
      ui::column({.align = FlexAlign::Start, .flexGrow = 1.0F}, std::move(title), std::move(subtitle)),
      ui::progressBar({
          .out = &inst.progress,
          .fill = fixedColorSpec(kWhite),
          .track = fixedColorSpec(white(0.2F)),
          .radius = kProgressHeight * 0.5F,
          .width = kProgressWidth,
          .height = kProgressHeight,
      }),
      ui::button({
          .out = &inst.secondary,
          .glyph = "bell-off",
          .glyphSize = Tokens::componentIslandIcon,
          .radius = kButton * 0.5F,
          .width = kButton,
          .height = kButton,
          .onClick =
              [this] {
                if (m_activity.has_value() && m_activity->secondaryAction) {
                  m_activity->secondaryAction();
                }
              },
      }),
      ui::button({
          .out = &inst.action,
          .glyph = "close",
          .glyphSize = Tokens::componentIslandIcon,
          .radius = kButton * 0.5F,
          .width = kButton,
          .height = kButton,
          .onClick =
              [this] {
                if (m_activity.has_value() && m_activity->action) {
                  m_activity->action();
                }
              },
      })
  );
  inst.title->setLineHeight(Tokens::typeTitle.size * Tokens::typeTitle.lineHeight);
  inst.subtitle->setLineHeight(Tokens::typeCaption.size * Tokens::typeCaption.lineHeight);
  inst.secondary->setCustomPalette(livePalette(false));
  inst.action->setCustomPalette(livePalette(false));
  inst.faceNode[static_cast<std::size_t>(Face::Live)] = content.get();
  tint->addChild(std::move(content));

  // Media hover face.
  const auto player = [this]() -> std::optional<MprisPlayerInfo> {
    const MprisService* mpris = m_mpris ? m_mpris() : nullptr;
    return mpris != nullptr ? mpris->activePlayer() : std::nullopt;
  };
  const auto control = [this, instPtr, player](bool (MprisService::*fn)(const std::string&)) {
    return [this, instPtr, player, fn] {
      MprisService* mpris = m_mpris ? m_mpris() : nullptr;
      if (const auto p = player(); mpris != nullptr && p.has_value()) {
        (mpris->*fn)(p->busName);
      }
      instPtr->surface->requestLayout();
    };
  };
  const auto mediaButton = [&](Button** out, const char* glyph, bool filled, std::function<void()> onClick) {
    auto button = ui::button({
        .out = out,
        .glyph = glyph,
        .glyphSize = 18.0F,
        .radius = kHoverButton * 0.5F,
        .width = kHoverButton,
        .height = kHoverButton,
        .onClick = std::move(onClick),
    });
    (*out)->setCustomPalette(filled ? livePalette(false) : ghostPalette());
    (*out)->setOnEnter([this, instPtr] { hoverEnter(*instPtr); });
    (*out)->setOnLeave([this, instPtr] { hoverLeave(*instPtr); });
    return button;
  };
  auto hoverArtPlaceholder = artPlaceholder(kHoverArt, 10.0F, 20.0F);
  inst.hoverArtPlaceholder = hoverArtPlaceholder.get();
  auto hoverMedia = ui::row(
      {.align = FlexAlign::Center, .gap = kHoverGap, .paddingH = kHoverPad, .width = inst.width, .height = kHeight},
      std::move(hoverArtPlaceholder),
      ui::image({.out = &inst.hoverArt, .fit = ImageFit::Cover, .radius = 10.0F, .width = kHoverArt, .height = kHoverArt, .visible = false}),
      ui::column(
          {.align = FlexAlign::Start, .flexGrow = 1.0F},
          ui::label({
              .out = &inst.hoverTitle,
              .fontSize = Tokens::typeTitle.size,
              .fontWeight = static_cast<FontWeight>(Tokens::typeTitle.weight),
              .color = fixedColorSpec(kWhite),
              .maxLines = 1,
              .autoScroll = true,
          }),
          ui::label({
              .out = &inst.hoverArtist,
              .fontSize = Tokens::typeCaption.size,
              .color = fixedColorSpec(white(0.7F)),
              .maxLines = 1,
              .autoScroll = true,
          })
      ),
      mediaButton(&inst.prev, "media-prev", false, control(&MprisService::previous)),
      mediaButton(&inst.playPause, "media-play", true, control(&MprisService::playPause)),
      mediaButton(&inst.next, "media-next", false, control(&MprisService::next))
  );
  inst.hoverTitle->setLineHeight(Tokens::typeTitle.size * Tokens::typeTitle.lineHeight);
  inst.hoverArtist->setLineHeight(Tokens::typeCaption.size * Tokens::typeCaption.lineHeight);
  inst.faceNode[static_cast<std::size_t>(Face::HoverMedia)] = hoverMedia.get();
  tint->addChild(std::move(hoverMedia));

  // Idle hover face: time, date, next event.
  auto hoverIdle = ui::row(
      {.align = FlexAlign::Center, .gap = 12.0F, .paddingH = 16.0F, .width = inst.width, .height = kHeight},
      ui::label({
          .out = &inst.idleTime,
          .fontSize = 22.0F,
          .fontWeight = FontWeight::SemiBold,
          .color = fixedColorSpec(kWhite),
          .maxLines = 1,
      }),
      ui::column(
          {.align = FlexAlign::Start, .flexGrow = 1.0F},
          ui::label({
              .out = &inst.idleDate,
              .fontSize = Tokens::typeCaption.size,
              .fontWeight = FontWeight::SemiBold,
              .color = fixedColorSpec(kWhite),
              .maxLines = 1,
          }),
          ui::label({
              .out = &inst.idleEvent,
              .fontSize = Tokens::typeCaption.size,
              .color = fixedColorSpec(white(0.7F)),
              .maxLines = 1,
          })
      )
  );
  inst.idleDate->setLineHeight(Tokens::typeCaption.size * Tokens::typeCaption.lineHeight);
  inst.idleEvent->setLineHeight(Tokens::typeCaption.size * Tokens::typeCaption.lineHeight);
  inst.faceNode[static_cast<std::size_t>(Face::HoverIdle)] = hoverIdle.get();
  tint->addChild(std::move(hoverIdle));

  // OSD face.
  auto osd = ui::row(
      {.align = FlexAlign::Center, .justify = FlexJustify::Center, .gap = 12.0F, .paddingH = 16.0F, .width = inst.width, .height = kHeight},
      ui::glyph({.out = &inst.osdGlyph, .glyphSize = 20.0F, .color = fixedColorSpec(kWhite)}),
      ui::progressBar({
          .out = &inst.osdBar,
          .fill = fixedColorSpec(kWhite),
          .track = fixedColorSpec(white(0.2F)),
          .radius = kOsdBarHeight * 0.5F,
          .width = kOsdBarWidth,
          .height = kOsdBarHeight,
      }),
      ui::label({
          .out = &inst.osdValue,
          .fontSize = Tokens::typeCaption.size,
          .fontWeight = FontWeight::SemiBold,
          .color = fixedColorSpec(kWhite),
          .minWidth = 36.0F,
          .maxLines = 1,
          .textAlign = TextAlign::End,
      })
  );
  inst.osdValue->setLineHeight(Tokens::typeCaption.size * Tokens::typeCaption.lineHeight);
  inst.faceNode[static_cast<std::size_t>(Face::Osd)] = osd.get();
  tint->addChild(std::move(osd));

  // Banner face.
  auto banner = ui::row(
      {.align = FlexAlign::Center, .gap = 12.0F, .paddingH = 12.0F, .width = inst.width, .height = kBannerHeight},
      iconCircle(&inst.bannerGlyph, kBannerGlyph),
      ui::column(
          {.align = FlexAlign::Start, .flexGrow = 1.0F},
          ui::label({
              .out = &inst.bannerTitle,
              .fontSize = Tokens::typeTitle.size,
              .fontWeight = static_cast<FontWeight>(Tokens::typeTitle.weight),
              .color = fixedColorSpec(kWhite),
              .maxLines = 1,
          }),
          ui::label({
              .out = &inst.bannerBody,
              .fontSize = Tokens::typeCaption.size,
              .color = fixedColorSpec(white(0.7F)),
              .maxLines = 2,
          })
      )
  );
  inst.bannerGlyph->setGlyph("bell");
  inst.bannerTitle->setLineHeight(Tokens::typeTitle.size * Tokens::typeTitle.lineHeight);
  inst.bannerBody->setLineHeight(Tokens::typeCaption.size * Tokens::typeCaption.lineHeight);
  inst.faceNode[static_cast<std::size_t>(Face::Banner)] = banner.get();
  tint->addChild(std::move(banner));

  auto ring = ui::box({.out = &inst.ring, .fill = clearColorSpec()});
  auto restArea = ui::inputArea({
      .onEnter = [this, instPtr](const InputArea::PointerData&) { hoverEnter(*instPtr); },
      .onLeave = [this, instPtr] { hoverLeave(*instPtr); },
      .onClick =
          [this, instPtr](const InputArea::PointerData&) {
            if (instPtr->face == Face::Live) {
              return;
            }
            if (instPtr->face == Face::Banner && m_banner.has_value()) {
              const BannerItem banner = *m_banner;
              endBanner();
              if (banner.defaultAction && m_notifications != nullptr) {
                const std::string token = m_wayland->requestActivationToken(instPtr->surface->wlSurface());
                if (m_notifications->invokeAction(banner.id, "default", token, true)) {
                  return;
                }
              }
            }
            if (m_open) {
              m_open(instPtr->output);
            }
          },
  });
  inst.restArea = restArea.get();

  // The area hosts the island so a hit on any of its nodes reaches it; the face buttons are deeper areas and win.
  // The input region limits the pointer to the visible card.
  restArea->setSize(width, height);
  restArea->addChild(std::move(shadowBox));
  restArea->addChild(std::move(material));
  restArea->addChild(std::move(tint));
  restArea->addChild(std::move(ring));
  restArea->addChild(ui::box({.out = &inst.edge}));
  inst.sceneRoot->addChild(std::move(restArea));
  inst.inputDispatcher.setSceneRoot(inst.sceneRoot.get());
  inst.inputDispatcher.setCursorShapeCallback([this](std::uint32_t serial, std::uint32_t shape) {
    m_wayland->setCursorShape(serial, shape);
  });
}

void LiveActivityIsland::loadArt(Instance& inst, Renderer& renderer, const std::string& url) {
  // The hover art decodes on first hover only; a failed or finished url is not retried.
  const bool wantHover = inst.face == Face::HoverMedia && !url.empty() && inst.hoverArtUrl != url;
  if (url != inst.artUrl) {
    inst.art->clear(renderer);
    inst.hoverArt->clear(renderer);
    inst.artUrl.clear();
    inst.hoverArtUrl.clear();
  } else if (!wantHover) {
    return;
  }
  if (url.empty()) {
    return;
  }
  const std::string path =
      mpris::resolveArtworkSource(m_http, m_pendingArt, url, refresher(), std::weak_ptr<void>(m_alive));
  if (path.empty()) {
    return;
  }
  bool ok = true;
  if (inst.artUrl != url) {
    inst.artUrl = url;
    ok = inst.art->setSourceFile(renderer, path, 64, true, true);
  }
  if (inst.face == Face::HoverMedia) {
    inst.hoverArtUrl = url;
    ok = inst.hoverArt->setSourceFile(renderer, path, 96, true, true) && ok;
  }
  if (!ok) {
    kLog.warn("center island artwork load failed url=\"{}\" path=\"{}\"", url, path);
  }
}

void LiveActivityIsland::applyContent(Instance& inst) {
  Renderer& renderer = inst.surface->renderTarget().renderer();

  inst.material->setFill(colorSpecFromRole(materialRole(), Style::glassAlpha(inst.space.backgroundOpacity)));
  inst.ring->setBorder(inst.space.border, std::max(inst.space.borderWidth, kIslandStrokeWidth));

  bool mic = false;
  bool camera = false;
  bool screen = false;
  PipeWireService* pipewire = m_pipewire ? m_pipewire() : nullptr;
  if (pipewire != nullptr && m_config != nullptr) {
    const ShellConfig::PrivacyConfig& privacy = m_config->config().shell.privacy;
    m_micFilter.update("shell.privacy.mic_filter_regex", privacy.micFilterRegex);
    m_camFilter.update("shell.privacy.cam_filter_regex", privacy.camFilterRegex);
    m_screenFilter.update("shell.privacy.screen_filter_regex", privacy.screenFilterRegex);
    for (const auto& capture : pipewire->privacyState().captures) {
      switch (capture.kind) {
      case PrivacyCaptureKind::Microphone:
        mic = mic || !m_micFilter.matches(capture.appName);
        break;
      case PrivacyCaptureKind::Camera:
        camera = camera || !m_camFilter.matches(capture.appName);
        break;
      case PrivacyCaptureKind::Screen:
        screen = screen || !m_screenFilter.matches(capture.appName);
        break;
      }
    }
  }
  const std::size_t unread = m_notifications != nullptr ? m_notifications->unreadNotificationCount() : 0;
  const MprisService* mpris = m_mpris ? m_mpris() : nullptr;
  const auto player = mpris != nullptr ? mpris->activePlayer() : std::nullopt;
  const bool playing = player.has_value() && player->playbackStatus == "Playing";
  m_date = dateText();
  inst.mediaGroup->setVisible(playing);
  inst.date->setVisible(!playing);
  inst.date->setText(m_date);
  inst.divider->setVisible(mic || camera || screen || unread > 0);
  inst.micDot->setVisible(mic);
  inst.cameraDot->setVisible(camera);
  inst.screenDot->setVisible(screen);
  inst.count->setVisible(unread > 0);
  if (unread > 0) {
    inst.count->setText(std::to_string(unread));
  }
  loadArt(inst, renderer, player.has_value() ? mpris::effectiveArtUrl(*player) : std::string{});
  const bool hasArt = inst.art->hasImage();
  inst.art->setVisible(hasArt);
  inst.artPlaceholder->setVisible(!hasArt);
  inst.hoverArt->setVisible(inst.hoverArt->hasImage());
  inst.hoverArtPlaceholder->setVisible(!inst.hoverArt->hasImage());
  Node* restNode = inst.faceNode[static_cast<std::size_t>(Face::Rest)];
  restNode->layout(renderer);
  inst.restWidth = std::min(inst.space.width, std::max(static_cast<float>(kRestWidth), restNode->width() + 2.0F * kRestPadding));
  inst.liveWidth = std::min(inst.width, std::max(kCollapsedWidth, inst.space.width));

  // Live card.
  const LiveActivity& a = m_shown;
  inst.icon->setGlyph(a.glyph);
  inst.title->setText(a.title);
  inst.subtitle->setText(a.subtitle);
  inst.subtitle->setVisible(!a.subtitle.empty());
  inst.progress->setVisible(a.progress.has_value() && inst.liveWidth >= kWidth);
  if (a.progress.has_value()) {
    inst.progress->setProgress(std::clamp(*a.progress, 0.0F, 1.0F));
  }
  inst.action->setVisible(!a.actionGlyph.empty());
  inst.action->setCustomPalette(livePalette(a.actionDestructive));
  if (!a.actionGlyph.empty()) {
    inst.action->setGlyph(a.actionGlyph);
  }
  inst.secondary->setVisible(!a.secondaryGlyph.empty());
  if (!a.secondaryGlyph.empty()) {
    inst.secondary->setGlyph(a.secondaryGlyph);
  }
  // The text takes what the icon, the bar, the buttons and their gaps leave.
  const bool showBar = inst.progress->visible();
  const int buttons = (a.actionGlyph.empty() ? 0 : 1) + (a.secondaryGlyph.empty() ? 0 : 1);
  const int gaps = 1 + (showBar ? 1 : 0) + buttons;
  const float textWidth = inst.liveWidth - 2.0F * kPadding - kIcon - static_cast<float>(gaps) * Tokens::componentLiveGap
      - (showBar ? kProgressWidth : 0.0F) - static_cast<float>(buttons) * kButton;
  inst.title->setMaxWidth(std::max(0.0F, textWidth));
  inst.subtitle->setMaxWidth(std::max(0.0F, textWidth));

  // Media hover face.
  const std::string trackTitle = player.has_value() ? (!player->title.empty() ? player->title : player->identity) : "";
  inst.hoverTitle->setText(trackTitle);
  inst.hoverArtist->setText(player.has_value() ? joinedArtists(player->artists) : "");
  inst.hoverArtist->setVisible(player.has_value() && !player->artists.empty());
  inst.playPause->setGlyph(playing ? "media-pause" : "media-play");
  const float textMax = std::max(60.0F, std::min(kHoverTextMax, inst.liveWidth - kHoverFixed));
  inst.hoverTitle->setMaxWidth(textMax);
  inst.hoverArtist->setMaxWidth(textMax);
  inst.hoverTitle->layout(renderer);
  inst.hoverArtist->layout(renderer);
  inst.faceWidth[static_cast<std::size_t>(Face::HoverMedia)] =
      kHoverFixed + std::min(textMax, std::max(inst.hoverTitle->width(), inst.hoverArtist->width()));

  // Idle hover face.
  inst.idleTime->setText(timeText());
  inst.idleDate->setText(fullDateText());
  std::string event;
  if (m_calendar != nullptr && m_calendar->enabled() && m_calendar->hasData()) {
    const auto now = std::chrono::system_clock::now();
    const CalendarEvent* next = nullptr;
    for (const CalendarEvent& e : m_calendar->snapshot().events) {
      if (e.end > now && (next == nullptr || e.start < next->start)) {
        next = &e;
      }
    }
    if (next != nullptr) {
      event = next->title;
      if (!next->allDay) {
        const std::time_t t = std::chrono::system_clock::to_time_t(next->start);
        std::tm local{};
        localtime_r(&t, &local);
        std::array<char, 16> buffer{};
        const std::size_t n = std::strftime(buffer.data(), buffer.size(), "%H:%M", &local);
        event = std::string(buffer.data(), n) + "  " + event;
      }
    }
  }
  inst.idleEvent->setText(event);
  inst.idleEvent->setVisible(!event.empty());
  inst.idleTime->layout(renderer);
  inst.idleDate->setMaxWidth(kIdleTextMax);
  inst.idleEvent->setMaxWidth(kIdleTextMax);
  inst.idleDate->layout(renderer);
  inst.idleEvent->layout(renderer);
  inst.faceWidth[static_cast<std::size_t>(Face::HoverIdle)] = 32.0F + 12.0F + inst.idleTime->width()
      + std::max(inst.idleDate->width(), event.empty() ? 0.0F : inst.idleEvent->width());

  // OSD face.
  if (m_osd.has_value()) {
    inst.osdGlyph->setGlyph(m_osd->icon);
    inst.osdValue->setText(m_osd->value);
    inst.osdBar->setVisible(m_osd->showProgress);
    inst.osdBar->setProgress(std::clamp(m_osd->progress, 0.0F, 1.0F));
  }
  inst.faceWidth[static_cast<std::size_t>(Face::Osd)] = m_osd.has_value() && !m_osd->showProgress ? 160.0F : kOsdWidth;

  // Banner face.
  if (m_banner.has_value()) {
    inst.bannerTitle->setText(m_banner->title);
    inst.bannerBody->setText(m_banner->body);
    inst.bannerBody->setVisible(!m_banner->body.empty());
  }
  const float bannerText = inst.liveWidth - 24.0F - kBannerGlyph - 12.0F;
  inst.bannerTitle->setMaxWidth(std::max(0.0F, bannerText));
  inst.bannerBody->setMaxWidth(std::max(0.0F, bannerText));

  for (std::size_t i = 1; i < kFaceCount; ++i) {
    const Rect r = faceRect(inst, static_cast<Face>(i));
    inst.faceNode[i]->setSize(r.w, r.h);
    inst.faceNode[i]->layout(renderer);
  }
  inst.to = faceRect(inst, inst.face);
  if (inst.reveal >= 1.0F) {
    inst.from = inst.to;
  }
  syncDsk(inst, true);
}

void LiveActivityIsland::applyReveal(Instance& inst, float reveal) {
  inst.reveal = reveal;
  if (inst.material == nullptr || inst.sceneRoot == nullptr) {
    return;
  }
  const float t = std::clamp(reveal, 0.0F, 1.0F);
  // Reduced motion: the size jumps and only the content cross-fades. The size follows the unclamped spring (overshoot).
  const float sizeT = MotionService::instance().enabled() ? reveal : 1.0F;
  const float maxW = inst.width;
  const float maxH = surfaceHeight();
  const float cw = std::clamp(std::round(lerp(inst.from.w, inst.to.w, sizeT)), 1.0F, maxW);
  const float ch = std::clamp(std::round(lerp(inst.from.h, inst.to.h, sizeT)), 1.0F, maxH);
  const Rect cur{
      .x = std::clamp(std::round(lerp(inst.from.x, inst.to.x, sizeT)), 0.0F, maxW - cw),
      .y = std::clamp(std::round(lerp(inst.from.y, inst.to.y, sizeT)), 0.0F, maxH - ch),
      .w = cw,
      .h = ch,
      .radius = std::max(0.0F, lerp(inst.from.radius, inst.to.radius, sizeT)),
  };
  inst.cur = cur;
  const Rect restR = faceRect(inst, Face::Rest);

  // With the compositor the fills span the surface and its clip morphs them; otherwise the fills are the island.
  const Rect box = inst.dsk != nullptr ? Rect{0.0F, 0.0F, inst.width, surfaceHeight(), cur.radius} : cur;
  for (Box* fill : {inst.material, inst.tint}) {
    fill->setPosition(box.x, box.y);
    fill->setSize(box.w, box.h);
    fill->setRadius(box.radius);
  }
  // The pill material shows at rest; every other face sits on the live tint.
  const float restMix = lerp(inst.prevFace == Face::Rest ? 1.0F : 0.0F, inst.face == Face::Rest ? 1.0F : 0.0F, t);
  inst.material->setOpacity(restMix);
  inst.tint->setOpacity(1.0F - restMix);
  inst.ring->setPosition(cur.x, cur.y);
  inst.ring->setSize(cur.w, cur.h);
  inst.ring->setRadius(cur.radius);
  inst.ring->setOpacity(restMix);
  inst.shadow->setPosition(cur.x, cur.y);
  inst.shadow->setSize(cur.w, cur.h);
  inst.shadow->setRadius(cur.radius);
  inst.shadow->setOpacity(restMix);
  {
    const float r = cur.radius;
    inst.edge->setStyle(RoundedRectStyle{.fill = islandEdgeColor(), .radius = Radii{kIslandStrokeWidth * 0.5F}});
    inst.edge->setPosition(cur.x + r * 0.55F, cur.y);
    inst.edge->setSize(std::max(0.0F, cur.w - r * 1.1F), kIslandStrokeWidth);
    inst.edge->setOpacity(restMix);
  }

  for (std::size_t i = 0; i < kFaceCount; ++i) {
    const auto f = static_cast<Face>(i);
    Node* node = inst.faceNode[i];
    float opacity = 0.0F;
    if (f == inst.face) {
      opacity = std::clamp((t - 0.4F) / 0.6F, 0.0F, 1.0F);
    } else if (f == inst.prevFace) {
      opacity = std::clamp(1.0F - t * 2.5F, 0.0F, 1.0F);
    }
    node->setVisible(opacity > 0.0F);
    node->setOpacity(opacity);
    if (f != Face::Rest) {
      node->setTransformOrigin(node->width() * 0.5F, node->height() * 0.5F);
      node->setScale(f == inst.face && MotionService::instance().enabled() ? 0.96F + 0.04F * t : 1.0F);
    }
    const Rect r = faceRect(inst, f);
    if (f == Face::Rest) {
      node->setPosition(
          std::round(restR.x + (restR.w - node->width()) * 0.5F - box.x),
          std::round(restR.y + (restR.h - node->height()) * 0.5F - box.y)
      );
    } else {
      node->setPosition(r.x - box.x, r.y - box.y);
    }
  }

  // The pill follows the bar's auto-hide fade; an expanded face stays.
  const float shown = lerp(inst.space.opacity, 1.0F, 1.0F - restMix);
  inst.sceneRoot->setOpacity(shown);

  const auto ix = static_cast<int>(cur.x);
  const auto iy = static_cast<int>(cur.y);
  const auto iw = std::max(1, static_cast<int>(cur.w));
  const auto ih = std::max(1, static_cast<int>(cur.h));
  if (shown > 0.01F) {
    inst.surface->setBlurRegion(Surface::tessellateRoundedRect(ix, iy, iw, ih, cur.radius));
  } else {
    inst.surface->clearBlurRegion();
  }
  inst.surface->setInputRegion(shown >= 0.5F ? std::vector<InputRect>{InputRect{ix, iy, iw, ih}} : std::vector<InputRect>{});
  inst.surface->requestRedraw();
}

void LiveActivityIsland::animateReveal(Instance& inst, float velocity) {
  if (inst.revealAnimId != 0) {
    inst.animations.cancel(inst.revealAnimId);
    inst.revealAnimId = 0;
  }
  inst.reveal = 0.0F;
  syncDsk(inst, true);
  auto* instPtr = &inst;
  const auto setter = [this, instPtr](float value) { applyReveal(*instPtr, value); };
  const auto done = [instPtr] {
    instPtr->revealAnimId = 0;
    instPtr->prevFace = instPtr->face;
    if (instPtr->face == Face::Rest && instPtr->dsk != nullptr) {
      instPtr->dsk->setElevation(DskSurfaceClient::Elevation::None);
    }
  };
  if (MotionService::instance().enabled()) {
    inst.revealAnimId = inst.animations.animateSpring(0.0F, 1.0F, revealSpring(inst.face != Face::Rest), setter, done, &inst, velocity);
  } else {
    inst.revealAnimId =
        inst.animations.animateTimer(0.0F, 1.0F, static_cast<float>(Tokens::motionFast), Easing::Linear, setter, done, &inst);
  }
  inst.surface->requestRedraw();
}

void LiveActivityIsland::syncBars() {
  const MprisService* mpris = m_mpris ? m_mpris() : nullptr;
  const auto player = mpris != nullptr ? mpris->activePlayer() : std::nullopt;
  const bool playing = !m_instances.empty() && player.has_value() && player->playbackStatus == "Playing";
  PipeWireSpectrum* spectrum = m_spectrum ? m_spectrum() : nullptr;
  if (!playing) {
    if (spectrum != nullptr && m_spectrumListener != 0) {
      spectrum->removeChangeListener(m_spectrumListener);
    }
    m_spectrumListener = 0;
    m_barsTimer.stop();
    return;
  }
  if (spectrum != nullptr && m_spectrumListener == 0) {
    m_spectrumListener = spectrum->addChangeListener(static_cast<int>(std::tuple_size_v<decltype(Instance::barBox)>), [] {});
  }
  if (!m_barsTimer.active()) {
    m_barsTimer.startRepeating(std::chrono::milliseconds(33), [this] { tickBars(); });
  }
}

void LiveActivityIsland::tickBars() {
  m_barsPhase += 0.033F;
  PipeWireSpectrum* spectrum = m_spectrum ? m_spectrum() : nullptr;
  const bool real = spectrum != nullptr && m_spectrumListener != 0 && !spectrum->idle();
  for (auto& inst : m_instances) {
    if (inst->bars == nullptr) {
      continue;
    }
    for (std::size_t i = 0; i < inst->barBox.size(); ++i) {
      const auto n = static_cast<float>(i);
      float target = 0.0F;
      if (real) {
        const auto& values = spectrum->values(m_spectrumListener);
        target = i < values.size() ? std::clamp(values[i], 0.0F, 1.0F) : 0.0F;
      } else {
        // No audio source: a smooth pseudo-random level that moves only while playing.
        target = 0.5F + 0.5F * std::sin(m_barsPhase * (5.1F + n * 1.7F) + n * 2.1F) * (0.65F + 0.35F * std::sin(m_barsPhase * 2.3F + n));
      }
      inst->barLevel[i] += (std::clamp(target, 0.0F, 1.0F) - inst->barLevel[i]) * 0.4F;
      const float h = 4.0F + inst->barLevel[i] * (kArt - 4.0F);
      inst->barBox[i]->setSize(kBarWidth, h);
      inst->barBox[i]->setPosition(n * (kBarWidth + kBarGap), std::round((kArt - h) * 0.5F));
    }
    if (inst->face == Face::Rest && inst->mediaGroup != nullptr && inst->mediaGroup->visible()) {
      inst->surface->requestRedraw();
    }
  }
}

bool LiveActivityIsland::onPointerEvent(const PointerEvent& event) {
  bool consumed = false;
  for (auto& inst : m_instances) {
    if (inst->surface == nullptr || inst->sceneRoot == nullptr) {
      continue;
    }
    const auto x = static_cast<float>(event.sx);
    const auto y = static_cast<float>(event.sy);
    const bool ours = event.surface == inst->surface->wlSurface();
    switch (event.type) {
    case PointerEvent::Type::Enter:
      if (ours) {
        inst->pointerInside = true;
        inst->inputDispatcher.pointerEnter(x, y, event.serial);
      }
      break;
    case PointerEvent::Type::Leave:
      if (ours) {
        inst->pointerInside = false;
        inst->inputDispatcher.pointerLeave();
      }
      break;
    case PointerEvent::Type::Motion:
      if (inst->pointerInside) {
        inst->inputDispatcher.pointerMotion(x, y, 0);
      }
      break;
    case PointerEvent::Type::Button:
      if (inst->pointerInside) {
        inst->inputDispatcher.pointerMotion(x, y, event.serial);
        inst->inputDispatcher.pointerButton(x, y, event.button, event.pressed, event.serial, event.time, event.touch);
        consumed = true;
      }
      break;
    case PointerEvent::Type::Axis:
      break;
    }
    if (inst->sceneRoot->paintDirty() || inst->sceneRoot->layoutDirty()) {
      inst->surface->requestRedraw();
    }
  }
  return consumed;
}

void LiveActivityIsland::syncVisibility() {
  std::unordered_set<wl_output*> now;
  for (const auto& inst : m_instances) {
    now.insert(inst->output);
  }
  const auto before = std::exchange(m_visibleOutputs, now);
  if (!m_visibilityCallback) {
    return;
  }
  for (wl_output* output : now) {
    if (!before.contains(output)) {
      m_visibilityCallback(output, true);
    }
  }
  for (wl_output* output : before) {
    if (!now.contains(output)) {
      m_visibilityCallback(output, false);
    }
  }
}
