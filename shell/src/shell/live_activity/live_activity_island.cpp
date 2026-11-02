#include "shell/live_activity/live_activity_island.h"

#include "config/config_service.h"
#include "config/config_types.h"
#include "core/deferred_call.h"
#include "core/log.h"
#include "core/ui_phase.h"
#include "calendar/calendar_service.h"
#include "dbus/mpris/mpris_art.h"
#include "dbus/mpris/mpris_service.h"
#include "net/uri.h"
#include "render/core/texture_manager.h"
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
  TOKENS_LIVE(kHeight, Tokens::componentIslandCenterLiveHeight);
  // Below this logical output width the island drops its progress bar, so it clears the bar's end island.
  constexpr std::int32_t kWideOutput = 1440;
  constexpr float kCompactWidth = 360.0F;
  // Narrowest live card.
  TOKENS_LIVE(kPadding, Tokens::componentIslandCenterLivePadding);
  TOKENS_LIVE(kIcon, Tokens::componentIslandCenterLiveLead);
  TOKENS_LIVE(kButton, Tokens::componentIslandCenterLiveLead);
  // M3: live activity progress -> full width, 4 high (rulebook 9.2)
  TOKENS_LIVE(kProgressHeight, Tokens::componentIslandCenterLevelHeight);
  // The expanded island is the scheme's thick glass: labels and fills are the scheme roles.
  ColorSpec liveLabel() { return colorSpecFromRole(ColorRole::Label1); }
  ColorSpec liveLabel2() { return colorSpecFromRole(ColorRole::Label2); }

  Button::ButtonPalette livePalette(bool destructive) {
    const ColorSpec bg = destructive ? colorSpecFromRole(ColorRole::Danger) : colorSpecFromRole(ColorRole::Fill2);
    const ColorSpec hover = destructive ? colorSpecFromRole(ColorRole::Danger, 0.85F) : colorSpecFromRole(ColorRole::Fill3);
    const ColorSpec label = destructive ? colorSpecFromRole(ColorRole::OnAccent) : liveLabel();
    const Button::ButtonStateColors normal{.bg = bg, .border = clearColorSpec(), .label = label};
    return Button::ButtonPalette{
        .normal = normal,
        .hover = {.bg = hover, .border = clearColorSpec(), .label = label},
        .pressed = {.bg = colorSpecFromRole(ColorRole::Fill3), .border = clearColorSpec(), .label = label},
        .disabled = normal,
    };
  }
  // Width left for text: 440 - 2 x 8 padding - 40 icon - 150 bar - 40 button - 3 x 12 gaps.
  constexpr float kTitleMaxWidth = 158.0F;
  // The compositor clip uses the same preset (syncDsk), so the fill and the clip stay together.
  desk::motion::SpringConfig revealSpring(bool grow) {
    const Tokens::Spring& token = grow ? Tokens::springMorph : Tokens::springCollapse;
    return {.damping = token.damping, .stiffness = token.stiffness};
  }
  // The spring the compositor really runs for the clip: protocol version < 3 maps the island curves to older ones.
  desk::motion::SpringConfig clipSpring(const DskSurfaceClient& dsk, bool grow) {
    const Tokens::Spring* token = &Tokens::springMove;
    switch (dsk.effectiveCurve(grow ? DskSurfaceClient::Curve::SpringIslandExpand : DskSurfaceClient::Curve::SpringIslandCollapse)) {
    case DskSurfaceClient::Curve::SpringSnappy: token = &Tokens::springSnap; break;
    case DskSurfaceClient::Curve::SpringBouncy: token = &Tokens::springOpen; break;
    case DskSurfaceClient::Curve::SpringIslandExpand: token = &Tokens::springMorph; break;
    case DskSurfaceClient::Curve::SpringIslandCollapse: token = &Tokens::springCollapse; break;
    default: break;
    }
    return {.damping = token->damping, .stiffness = token->stiffness};
  }
  // Shadow level (dsk_surface_v1 elevation property): 1 is the float plane (elevation.e2), 2 the over plane (e3). The
  // rest pill floats like the bar islands.
  constexpr double kRestElevation = 1.0;
  constexpr double kExpandedElevation = 2.0;
  constexpr float kMirrorMaxMs = 4000.0F;
  constexpr float kFaceScale = 0.92F;
  constexpr float kMinCardWidth = 240.0F;
  constexpr float kMinCardHeight = 56.0F;
  constexpr float kMaxCardHeight = 96.0F; // surface bound: banner with caption, title and two body lines is 88
  constexpr float kCaptionGap = 8.0F;
  float ceil4(float v) { return std::ceil(v / 4.0F) * 4.0F; }
  // The island lays out in tokens px; the surface, clip, blur and input regions are that times the bar's content scale.
  float islandScale(const CenterIslandSpace& space) { return std::clamp(space.scale, 0.25F, 4.0F); }
  constexpr auto kStartWait = std::chrono::milliseconds(500);
  TOKENS_LIVE(kLiveRadius, Tokens::radiusR28);
  TOKENS_LIVE(kDot, Tokens::componentIslandPrivacyDot);
  TOKENS_LIVE(kRestWidth, Tokens::componentIslandCenterRestWidth);
  constexpr float kArt = 20.0F;
  constexpr float kArtRadius = kArt * 0.5F;
  constexpr float kRestPadding = 8.0F;
  constexpr float kRestMaxWidth = 280.0F;
  constexpr std::int32_t kShadowPad = ui::surface_shadow::kBlurRadius + 2;
  TOKENS_LIVE(kBannerHeight, Tokens::componentIslandCenterBannerHeight);
  TOKENS_LIVE(kBannerRadius, Tokens::componentIslandCenterBannerRadius);
  TOKENS_LIVE(kBannerPad, Tokens::componentIslandCenterBannerPadding);
  TOKENS_LIVE(kExpandedWidth, Tokens::componentIslandCenterExpandedWidth);
  TOKENS_LIVE(kOsdWidth, Tokens::componentIslandCenterOsdWidth);
  TOKENS_LIVE(kOsdHeight, Tokens::componentIslandCenterOsdHeight);
  constexpr float kOsdBarHeight = 4.0F;
  TOKENS_LIVE(kHoverArt, Tokens::componentIslandCenterArt);
  TOKENS_LIVE(kHoverArtRadius, Tokens::componentIslandCenterArtRadius);
  constexpr float kHoverButton = 32.0F;
  constexpr float kHoverGap = 12.0F;
  constexpr float kHoverButtonGap = 8.0F;
  // Art, three buttons, their gaps and the padding: what the media face needs besides its text.
  float hoverFixed() { return 2.0F * kBannerPad + kHoverArt + 2.0F * kHoverGap + 3.0F * kHoverButton + 2.0F * kHoverButtonGap; }
  constexpr float kIdleTextMax = 220.0F;
  constexpr float kBannerGlyph = 32.0F;
  constexpr float kBarGap = 2.0F;
  constexpr float kBarWidth = 3.0F;
  constexpr auto kCollapseDelay = std::chrono::milliseconds(300);
  constexpr auto kOsdTime = std::chrono::milliseconds(1500);
  constexpr auto kBannerTime = std::chrono::milliseconds(5000);

  Button::ButtonPalette ghostPalette() {
    const Button::ButtonStateColors normal{.bg = clearColorSpec(), .border = clearColorSpec(), .label = liveLabel()};
    return Button::ButtonPalette{
        .normal = normal,
        .hover = {.bg = colorSpecFromRole(ColorRole::Fill3), .border = clearColorSpec(), .label = liveLabel()},
        .pressed = {.bg = colorSpecFromRole(ColorRole::Fill3), .border = clearColorSpec(), .label = liveLabel()},
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
        .appName = notificationDisplayAppName(n),
        .time = timeText(),
        .icon = n.icon.value_or(std::string{}),
        .desktopEntry = n.desktopEntry.value_or(std::string{}),
        .imageData = n.imageData,
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
    if (outputGone || !space.has_value() || space->bottom != inst->space.bottom || (space->height > 0.0F && std::abs(islandScale(*space) - inst->scale) > 0.001F)) {
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
          space->bottom ? LayerShellAnchor::Bottom : LayerShellAnchor::Top, static_cast<std::uint32_t>(std::ceil(width * inst->scale)),
          static_cast<std::uint32_t>(std::ceil(surfaceHeight(inst->scale) * inst->scale))
      );
      inst->dskTarget.reset();
    }
    if (space->edgeMargin != inst->space.edgeMargin) {
      inst->surface->setMargins(space->bottom ? 0 : surfaceMargin(*space), 0, space->bottom ? surfaceMargin(*space) : 0, 0);
      inst->pad = static_cast<float>(std::min(space->edgeMargin, kShadowPad)) / inst->scale;
      inst->dskTarget.reset();
    }
    if (space->height > 0.0F) {
      inst->space = *space;
    }
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
    if (!space.has_value() || space->height <= 0.0F) {
      continue;
    }

    auto inst = std::make_unique<Instance>();
    inst->output = output.output;
    inst->space = *space;
    inst->scale = islandScale(*space);
    inst->width = output.logicalWidth >= kWideOutput ? kWidth : kCompactWidth;
    kLog.info("center island surface width {} (logical output width {})", inst->width, output.logicalWidth);
    inst->surface = std::make_unique<LayerSurface>(
        *m_wayland,
        LayerSurfaceConfig{
            .nameSpace = "noctalia-live-activity",
            .layer = LayerShellLayer::Overlay,
            .anchor = space->bottom ? LayerShellAnchor::Bottom : LayerShellAnchor::Top,
            .width = static_cast<std::uint32_t>(std::ceil(inst->width * inst->scale)),
            .height = static_cast<std::uint32_t>(std::ceil(surfaceHeight(inst->scale) * inst->scale)),
            // -1: placed from the screen edge, over the bar's exclusive zone.
            .exclusiveZone = -1,
            .marginTop = space->bottom ? 0 : surfaceMargin(*space),
            .marginBottom = space->bottom ? surfaceMargin(*space) : 0,
            .keyboard = LayerShellKeyboard::None,
            .defaultWidth = static_cast<std::uint32_t>(std::ceil(inst->width * inst->scale)),
            .defaultHeight = static_cast<std::uint32_t>(std::ceil(surfaceHeight(inst->scale) * inst->scale)),
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
    inst->liveWidth = std::min(inst->width, static_cast<float>(kExpandedWidth));
    inst->restWidth = std::min(space->width / inst->scale, static_cast<float>(kRestWidth));
    inst->pad = static_cast<float>(std::min(space->edgeMargin, kShadowPad)) / inst->scale;
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


float LiveActivityIsland::surfaceHeight(float scale) const {
  return kMaxCardHeight + static_cast<float>(kShadowPad) / scale;
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
    h = std::min(inst.space.height / inst.scale, static_cast<float>(kHeight));
    w = inst.restWidth;
    radius = h * 0.5F;
    break;
  case Face::HoverIdle:
    w = std::max(inst.restWidth, std::min(inst.faceWidth[static_cast<std::size_t>(face)], inst.liveWidth));
    break;
  case Face::Osd:
  case Face::HoverMedia:
  case Face::Banner:
  case Face::Live:
    // The card sizes to its content: 240..360 wide, 56 or more high on the 4 grid; a 56 card is a capsule.
    w = std::clamp(inst.faceWidth[static_cast<std::size_t>(face)], kMinCardWidth, inst.liveWidth);
    h = std::max(kMinCardHeight, inst.faceHeight[static_cast<std::size_t>(face)]);
    radius = h <= kMinCardHeight ? h * 0.5F : static_cast<float>(kBannerRadius);
    break;
  }
  return Rect{
      .x = std::round((inst.width - w) * 0.5F),
      .y = inst.space.bottom ? surfaceHeight(inst.scale) - inst.pad - h : inst.pad,
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
  if (inst.revealAnimId != 0 && !mirrored(inst)) {
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
  const double k = inst.scale;
  const DskSurfaceClient::Value clip{r.x * k, r.y * k, r.w * k, r.h * k};
  const DskSurfaceClient::Value radius{r.radius * k, 0.0, 0.0, 0.0};
  const bool level = inst.dsk->hasElevationProperty();
  const double elevation = expanded ? kExpandedElevation : kRestElevation * (inst.space.restless ? 0.0F : inst.space.opacity);
  const bool jump = !inst.dskTarget.has_value() || !animate || !MotionService::instance().enabled();
  // A jump while a morph runs (surface size or margin change) ends that morph: the shell settles on the new rect.
  bool settle = false;
  if (jump) {
    inst.dsk->set(DskSurfaceClient::Property::Clip, clip);
    inst.dsk->set(DskSurfaceClient::Property::Radius, radius);
    inst.mirror.active = false;
    if (inst.revealAnimId != 0) {
      inst.animations.cancel(inst.revealAnimId);
      inst.revealAnimId = 0;
      inst.reveal = 1.0F;
      inst.from = inst.to;
      settle = true;
    }
  } else {
    const auto curve = expanded ? DskSurfaceClient::Curve::SpringIslandExpand : DskSurfaceClient::Curve::SpringIslandCollapse;
    startMirror(inst, r, expanded, true);
    if (!inst.mirror.fadePending && inst.mirror.fade.settled()) {
      // A retarget outside animateReveal (content width): the faces stay shown while the shape moves.
      inst.mirror.fade = desk::motion::Spring(1.0, 1.0, clipSpring(*inst.dsk, expanded));
    }
    ensureMirrorDriver(inst);
    auto* instPtr = &inst;
    const std::uint32_t generation = inst.mirror.generation;
    inst.dsk->animate(DskSurfaceClient::Property::Clip, curve, clip, {}, [instPtr, generation](std::uint32_t timeMs) {
      Instance::Mirror& m = instPtr->mirror;
      if (m.generation != generation || !m.waiting) {
        return;
      }
      m.waiting = false;
      // A retargeted compositor spring already steps one frame on its first tick.
      m.startMs = m.retarget ? timeMs - m.frameMs : timeMs;
      m.shapeSec = 0.0;
      if (m.fadePending) {
        m.fadePending = false;
        m.fadeStartMs = timeMs;
        m.fadeSec = 0.0;
      }
      instPtr->surface->requestRedraw();
    });
    inst.dsk->animate(DskSurfaceClient::Property::Radius, curve, radius);
    if (level) {
      inst.dsk->animate(DskSurfaceClient::Property::Elevation, curve, {elevation, 0.0, 0.0, 0.0});
    }
  }
  if (level) {
    if (jump) {
      inst.dsk->set(DskSurfaceClient::Property::Elevation, {elevation, 0.0, 0.0, 0.0});
    }
    inst.dskLevel = elevation;
  } else if (expanded) {
    inst.dsk->setElevation(DskSurfaceClient::Elevation::E3);
  } else if (jump) {
    inst.dsk->setElevation(DskSurfaceClient::Elevation::None);
  }
  inst.dskTarget = r;
  if (settle) {
    finishReveal(inst);
    applyReveal(inst, 1.0F);
  }
  inst.surface->requestRedraw();
}

bool LiveActivityIsland::mirrored(const Instance& inst) { return inst.dsk != nullptr && MotionService::instance().enabled(); }

namespace {
  std::uint32_t monotonicMs() {
    return static_cast<std::uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()
    );
  }
  // Wrap-safe seconds from startMs to nowMs, never negative.
  double secondsSince(std::uint32_t nowMs, std::uint32_t startMs) {
    return std::max(0, static_cast<std::int32_t>(nowMs - startMs)) / 1000.0;
  }
} // namespace

// Same springs as the compositor's clip and radius (SurfaceMotion::animate): a running one keeps its velocity.
void LiveActivityIsland::startMirror(Instance& inst, const Rect& target, bool grow, bool waitForStart) {
  Instance::Mirror& m = inst.mirror;
  const auto config = clipSpring(*inst.dsk, grow);
  const std::array<double, 5> to{target.x, target.y, target.w, target.h, target.radius};
  const std::array<double, 5> now{inst.cur.x, inst.cur.y, inst.cur.w, inst.cur.h, inst.cur.radius};
  for (std::size_t i = 0; i < to.size(); ++i) {
    m.shape[i] = m.active ? desk::motion::Spring(m.shape[i].value(), to[i], config, m.shape[i].velocity())
                          : desk::motion::Spring(now[i], to[i], config);
  }
  m.retarget = m.active;
  m.active = true;
  m.waiting = waitForStart;
  m.requested = std::chrono::steady_clock::now();
  m.startMs = monotonicMs();
  m.shapeSec = 0.0;
  ++m.generation;
}

void LiveActivityIsland::stepMirror(Instance& inst) {
  Instance::Mirror& m = inst.mirror;
  if (!m.active) {
    return;
  }
  const std::uint32_t now = monotonicMs();
  const std::uint32_t interval = now - m.lastStepMs;
  if (m.lastStepMs != 0 && interval > 0 && interval <= 50) {
    m.frameMs = interval;
  }
  m.lastStepMs = now;
  if (m.waiting &&std::chrono::steady_clock::now() - m.requested > kStartWait) {
    m.waiting = false;
    m.startMs = now;
    m.shapeSec = 0.0;
    if (m.fadePending) {
      m.fadePending = false;
      m.fadeStartMs = now;
      m.fadeSec = 0.0;
    }
  }
  if (m.waiting) {
    inst.surface->requestRedraw();
    return;
  }
  const double shapeSec = secondsSince(now, m.startMs);
  bool settled = true;
  for (auto& spring : m.shape) {
    spring.step(shapeSec - m.shapeSec);
    settled = settled && spring.settled();
  }
  m.shapeSec = shapeSec;
  const double fadeSec = secondsSince(now, m.fadeStartMs);
  m.fade.step(fadeSec - m.fadeSec);
  m.fadeSec = fadeSec;
  settled = settled && m.fade.settled() && !m.fadePending;
  if (settled) {
    m.active = false;
    inst.reveal = 1.0F;
    inst.from = inst.to;
    applyReveal(inst, 1.0F);
    finishReveal(inst);
    return;
  }
  applyReveal(inst, static_cast<float>(m.fade.value()));
}

// The timer only drives stepMirror; the mirror springs end it.
void LiveActivityIsland::ensureMirrorDriver(Instance& inst) {
  if (inst.revealAnimId != 0) {
    return;
  }
  auto* instPtr = &inst;
  // Timer: continuous driver for the mirrored compositor motion, capped at kMirrorMaxMs.
  inst.revealAnimId = inst.animations.animateTimer(
      0.0F, 1.0F, kMirrorMaxMs, Easing::Linear, [this, instPtr](float) { stepMirror(*instPtr); }, {}, &inst
  );
}

void LiveActivityIsland::finishReveal(Instance& inst) {
  // The driver timer is running this call: cancel it from outside its own tick.
  const AnimationManager::Id driver = inst.revealAnimId;
  inst.revealAnimId = 0;
  inst.prevFace = inst.face;
  auto* instPtr = &inst;
  if (driver != 0) {
    inst.mirrorStop.start(std::chrono::milliseconds(1), [instPtr, driver] { instPtr->animations.cancel(driver); });
  }
  if (inst.face == Face::Rest && inst.dsk != nullptr && !inst.dsk->hasElevationProperty()) {
    inst.dsk->setElevation(DskSurfaceClient::Elevation::None);
  }
  if (inst.face != Face::Banner && inst.dragOffset != 0.0F && !inst.dragTimer.active()) {
    inst.dragOffset = 0.0F;
    inst.dsk->set(DskSurfaceClient::Property::Translate, {0.0, 0.0, 0.0, 0.0});
    inst.surface->requestRedraw();
  }
}

void LiveActivityIsland::prepareFrame(Instance& inst, bool needsUpdate, bool needsLayout) {
  const auto width = static_cast<float>(inst.surface->width());
  const auto height = static_cast<float>(inst.surface->height());
  if (width <= 0.0F || height <= 0.0F) {
    return;
  }
  m_renderContext->makeCurrent(inst.surface->renderTarget());
  UiPhaseScope layoutPhase(UiPhase::Layout);
  const bool build = inst.sceneRoot == nullptr || std::round(inst.sceneRoot->width() * inst.scale) != width
      || std::round(inst.sceneRoot->height() * inst.scale) != height;
  if (build) {
    buildScene(inst, width / inst.scale, height / inst.scale);
  }
  if (build || needsUpdate || needsLayout) {
    applyContent(inst);
    applyReveal(inst, inst.reveal);
  }
}

void LiveActivityIsland::buildScene(Instance& inst, float width, float height) {
  inst.sceneRoot = ui::node({});
  inst.bannerIconKey.clear();
  inst.sceneRoot->setSize(width, height);
  inst.sceneRoot->setTransformOrigin(0.0F, 0.0F);
  inst.sceneRoot->setScale(inst.scale);
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
      .fill = colorSpecFromRole(panelMaterialRole(), Style::glassAlpha(kTintThickAlpha)),
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
      ui::label({
          .out = &inst.mediaTitle,
          .fontSize = Tokens::typeFootnote.size,
          .color = colorSpecFromRole(ColorRole::Label1),
          .maxLines = 1,
      }),
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
  inst.mediaTitle->setLineHeight(Tokens::typeFootnote.line);
  inst.count->setLineHeight(Tokens::typeBody.line);
  inst.date->setLineHeight(Tokens::typeBody.line);
  inst.faceNode[static_cast<std::size_t>(Face::Rest)] = restContent.get();
  material->addChild(std::move(restContent));

  // Live card (call, recording, transfer).
  auto title = ui::label({
      .out = &inst.title,
      .fontSize = Tokens::typeHeadline.size,
      .fontWeight = static_cast<FontWeight>(Tokens::typeHeadline.weight),
      .color = liveLabel(),
      .maxWidth = kTitleMaxWidth,
      .maxLines = 1,
  });
  auto subtitle = ui::label({
      .out = &inst.subtitle,
      .fontSize = Tokens::typeFootnote.size,
      .color = liveLabel2(),
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
          .gap = Tokens::componentIslandGroupGap,
          .paddingH = kPadding,
          .width = inst.width,
          .height = kHeight,
      },
      iconCircle(&inst.icon, kIcon),
      ui::column(
          {.align = FlexAlign::Stretch, .flexGrow = 1.0F},
          std::move(title),
          std::move(subtitle),
          ui::progressBar({
              .out = &inst.progress,
              .fill = colorSpecFromRole(ColorRole::Accent),
              .track = colorSpecFromRole(ColorRole::Fill2),
              .radius = kProgressHeight * 0.5F,
              .height = kProgressHeight,
          })
      ),
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
  inst.title->setLineHeight(Tokens::typeHeadline.line);
  inst.subtitle->setLineHeight(Tokens::typeFootnote.line);
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
  auto hoverArtPlaceholder = artPlaceholder(kHoverArt, kHoverArtRadius, 20.0F);
  inst.hoverArtPlaceholder = hoverArtPlaceholder.get();
  auto hoverRow = ui::row(
      {.align = FlexAlign::Center, .gap = kHoverGap, .width = inst.width, .height = kHoverArt},
      std::move(hoverArtPlaceholder),
      ui::image({.out = &inst.hoverArt, .fit = ImageFit::Cover, .radius = kHoverArtRadius, .width = kHoverArt, .height = kHoverArt, .visible = false}),
      ui::column(
          {.align = FlexAlign::Start, .flexGrow = 1.0F},
          ui::label({
              .out = &inst.hoverTitle,
              .fontSize = Tokens::typeHeadline.size,
              .fontWeight = static_cast<FontWeight>(Tokens::typeHeadline.weight),
              .color = liveLabel(),
              .maxLines = 1,
              .autoScroll = true,
          }),
          ui::label({
              .out = &inst.hoverArtist,
              .fontSize = Tokens::typeFootnote.size,
              .color = liveLabel2(),
              .maxLines = 1,
              .autoScroll = true,
          })
      ),
      ui::row(
          {.align = FlexAlign::Center, .gap = kHoverButtonGap},
          mediaButton(&inst.prev, "media-prev", false, control(&MprisService::previous)),
          mediaButton(&inst.playPause, "media-play", true, control(&MprisService::playPause)),
          mediaButton(&inst.next, "media-next", false, control(&MprisService::next))
      )
  );
  auto hoverMedia = ui::column(
      {.align = FlexAlign::Stretch, .gap = kHoverGap, .paddingV = kBannerPad, .paddingH = kBannerPad, .width = inst.width, .height = kBannerHeight},
      std::move(hoverRow),
      ui::progressBar({
          .out = &inst.hoverProgress,
          .fill = colorSpecFromRole(ColorRole::Accent),
          .track = colorSpecFromRole(ColorRole::Fill2),
          .radius = kProgressHeight * 0.5F,
          .height = kProgressHeight,
      })
  );
  inst.hoverTitle->setLineHeight(Tokens::typeHeadline.line);
  inst.hoverArtist->setLineHeight(Tokens::typeFootnote.line);
  inst.faceNode[static_cast<std::size_t>(Face::HoverMedia)] = hoverMedia.get();
  tint->addChild(std::move(hoverMedia));

  // Idle hover face: time, date, next event.
  auto hoverIdle = ui::row(
      {.align = FlexAlign::Center, .gap = 12.0F, .paddingH = 16.0F, .width = inst.width, .height = kHeight},
      ui::label({
          .out = &inst.idleTime,
          .fontSize = 22.0F,
          .fontWeight = FontWeight::SemiBold,
          .color = liveLabel(),
          .maxLines = 1,
      }),
      ui::column(
          {.align = FlexAlign::Start, .flexGrow = 1.0F},
          ui::label({
              .out = &inst.idleDate,
              .fontSize = Tokens::typeFootnote.size,
              .fontWeight = FontWeight::SemiBold,
              .color = liveLabel(),
              .maxLines = 1,
          }),
          ui::label({
              .out = &inst.idleEvent,
              .fontSize = Tokens::typeFootnote.size,
              .color = liveLabel2(),
              .maxLines = 1,
          })
      )
  );
  inst.idleDate->setLineHeight(Tokens::typeFootnote.line);
  inst.idleEvent->setLineHeight(Tokens::typeFootnote.line);
  inst.faceNode[static_cast<std::size_t>(Face::HoverIdle)] = hoverIdle.get();
  tint->addChild(std::move(hoverIdle));

  // OSD face.
  auto osd = ui::row(
      {.align = FlexAlign::Center, .justify = FlexJustify::Center, .gap = 12.0F, .paddingH = 12.0F, .width = inst.width, .height = kOsdHeight},
      ui::glyph({.out = &inst.osdGlyph, .glyphSize = Tokens::componentIslandIcon, .color = liveLabel()}),
      ui::progressBar({
          .out = &inst.osdBar,
          .fill = colorSpecFromRole(ColorRole::SliderFill),
          .track = colorSpecFromRole(ColorRole::SliderTrack),
          .radius = kOsdBarHeight * 0.5F,
          .height = kOsdBarHeight,
          .flexGrow = 1.0F,
      }),
      ui::label({
          .out = &inst.osdValue,
          .fontSize = Tokens::typeCaption.size,
          .fontWeight = FontWeight::SemiBold,
          .fontFeatures = "tnum",
          .color = liveLabel(),
          .minWidth = 40.0F, // reserves the width of "100%", so the level bar does not move with the digits
          .maxLines = 1,
          .textAlign = TextAlign::End,
      })
  );
  inst.osdValue->setLineHeight(Tokens::typeCaption.line);
  inst.faceNode[static_cast<std::size_t>(Face::Osd)] = osd.get();
  tint->addChild(std::move(osd));

  // Banner face: the app icon (image-data, icon, desktop entry) over the accent glyph circle fallback.
  auto bannerIcon = ui::node({.width = kBannerGlyph, .height = kBannerGlyph});
  auto bannerCircle = iconCircle(&inst.bannerGlyph, kBannerGlyph);
  inst.bannerCircle = bannerCircle.get();
  bannerIcon->addChild(std::move(bannerCircle));
  bannerIcon->addChild(ui::image({
      .out = &inst.bannerImage,
      .fit = ImageFit::Cover,
      .radius = Tokens::radiusR8,
      .width = kBannerGlyph,
      .height = kBannerGlyph,
      .visible = false,
  }));
  auto banner = ui::row(
      {.align = FlexAlign::Center, .gap = 12.0F, .paddingH = kBannerPad, .width = inst.width, .height = kBannerHeight},
      std::move(bannerIcon),
      ui::column(
          {.align = FlexAlign::Stretch, .flexGrow = 1.0F},
          ui::row(
              {.align = FlexAlign::Center, .gap = kCaptionGap},
              ui::label({
                  .out = &inst.bannerApp,
                  .fontSize = Tokens::typeCaption.size,
                  .fontWeight = static_cast<FontWeight>(Tokens::typeCaption.weight),
                  .color = liveLabel2(),
                  .maxLines = 1,
                  .flexGrow = 1.0F,
              }),
              ui::label({
                  .out = &inst.bannerTime,
                  .fontSize = Tokens::typeCaption.size,
                  .fontWeight = static_cast<FontWeight>(Tokens::typeCaption.weight),
                  .color = liveLabel2(),
                  .maxLines = 1,
              })
          ),
          ui::label({
              .out = &inst.bannerTitle,
              .fontSize = Tokens::typeHeadline.size,
              .fontWeight = static_cast<FontWeight>(Tokens::typeHeadline.weight),
              .color = liveLabel(),
              .maxLines = 1,
          }),
          ui::label({
              .out = &inst.bannerBody,
              .fontSize = Tokens::typeFootnote.size,
              .color = liveLabel2(),
              .maxLines = 2,
          })
      )
  );
  inst.bannerGlyph->setGlyph("bell");
  inst.bannerApp->setLineHeight(Tokens::typeCaption.line);
  inst.bannerTime->setLineHeight(Tokens::typeCaption.line);
  inst.bannerTitle->setLineHeight(Tokens::typeHeadline.line);
  inst.bannerBody->setLineHeight(Tokens::typeFootnote.line);
  inst.faceNode[static_cast<std::size_t>(Face::Banner)] = banner.get();
  tint->addChild(std::move(banner));

  auto ring = ui::box({.out = &inst.ring, .fill = clearColorSpec()});
  auto restArea = ui::inputArea({
      .onEnter = [this, instPtr](const InputArea::PointerData&) { hoverEnter(*instPtr); },
      .onLeave = [this, instPtr] { hoverLeave(*instPtr); },
      .onMotion = [this, instPtr](const InputArea::PointerData& d) { bannerDragMove(*instPtr, d.localY); },
      .onPress =
          [this, instPtr](const InputArea::PointerData& d) {
            if (d.button == BTN_LEFT) {
              d.pressed ? bannerDragStart(*instPtr, d.localY) : bannerDragEnd(*instPtr);
            }
          },
      .onClick =
          [this, instPtr](const InputArea::PointerData&) {
            if (std::exchange(instPtr->dragMoved, false)) {
              return;
            }
            if (instPtr->face == Face::Live) {
              if (m_activity.has_value() && m_activity->open) {
                m_activity->open(instPtr->output);
              }
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
  inst.ring->setBorder(inst.space.border, islandStrokeWidth(inst.space.borderWidth));

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
  inst.mediaTitle->setText(playing ? (!player->title.empty() ? player->title : player->identity) : std::string{});
  inst.mediaTitle->setMaxWidth(kRestMaxWidth);
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
  const float restCap = inst.space.width > 0.0F ? std::min(kRestMaxWidth, inst.space.width / inst.scale) : kRestMaxWidth;
  if (playing && restNode->width() + 2.0F * kRestPadding > restCap) {
    // The title takes what the art, the waveform and the privacy dots leave.
    const float over = restNode->width() + 2.0F * kRestPadding - restCap;
    inst.mediaTitle->setMaxWidth(std::max(24.0F, inst.mediaTitle->width() - over));
    restNode->layout(renderer);
  }
  inst.restWidth = std::min({restCap, std::max(static_cast<float>(kRestWidth), restNode->width() + 2.0F * kRestPadding)});
  inst.liveWidth = std::min(inst.width, static_cast<float>(kExpandedWidth));

  // Live card.
  const LiveActivity& a = m_shown;
  inst.icon->setGlyph(a.glyph);
  inst.title->setText(a.title);
  inst.subtitle->setText(a.subtitle);
  inst.subtitle->setVisible(!a.subtitle.empty());
  inst.progress->setVisible(a.progress.has_value());
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
  const float liveFixed = 2.0F * kPadding + kIcon + static_cast<float>(1 + buttons) * Tokens::componentIslandGroupGap
      + static_cast<float>(buttons) * kButton;
  inst.title->setMaxWidth(std::max(0.0F, inst.liveWidth - liveFixed));
  inst.subtitle->setMaxWidth(std::max(0.0F, inst.liveWidth - liveFixed));
  inst.title->layout(renderer);
  inst.subtitle->layout(renderer);
  {
    const float textW = std::max(inst.title->width(), inst.subtitle->visible() ? inst.subtitle->width() : 0.0F);
    const float w = std::clamp(liveFixed + textW, kMinCardWidth, inst.liveWidth);
    inst.faceWidth[static_cast<std::size_t>(Face::Live)] = w;
    inst.title->setMaxWidth(w - liveFixed);
    inst.subtitle->setMaxWidth(w - liveFixed);
    const float column = Tokens::typeHeadline.line + (inst.subtitle->visible() ? Tokens::typeFootnote.line : 0.0F)
        + (showBar ? 2.0F * kProgressHeight : 0.0F);
    inst.faceHeight[static_cast<std::size_t>(Face::Live)] = ceil4(2.0F * kPadding + column);
  }

  // Media hover face.
  const std::string trackTitle = player.has_value() ? (!player->title.empty() ? player->title : player->identity) : "";
  inst.hoverTitle->setText(trackTitle);
  inst.hoverArtist->setText(player.has_value() ? joinedArtists(player->artists) : "");
  inst.hoverArtist->setVisible(player.has_value() && !player->artists.empty());
  inst.playPause->setGlyph(playing ? "media-pause" : "media-play");
  const float textMax = std::max(60.0F, inst.liveWidth - hoverFixed());
  inst.hoverTitle->setMaxWidth(textMax);
  inst.hoverArtist->setMaxWidth(textMax);
  inst.hoverTitle->layout(renderer);
  inst.hoverArtist->layout(renderer);
  syncHoverProgress(inst);
  inst.faceWidth[static_cast<std::size_t>(Face::HoverMedia)] =
      hoverFixed() + std::max(inst.hoverTitle->width(), inst.hoverArtist->visible() ? inst.hoverArtist->width() : 0.0F);
  inst.faceHeight[static_cast<std::size_t>(Face::HoverMedia)] = static_cast<float>(kBannerHeight);

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
    inst.osdValue->setText(m_osd->value.empty() && m_osd->showProgress ? std::to_string(static_cast<int>(std::lround(std::clamp(m_osd->progress, 0.0F, 1.0F) * 100.0F))) + "%" : m_osd->value);
    inst.osdBar->setVisible(m_osd->showProgress);
    const float osdTarget = std::clamp(m_osd->progress, 0.0F, 1.0F);
    if (inst.osdAnimId != 0 && inst.animations.retarget(inst.osdAnimId, osdTarget)) {
      // A running spring keeps its velocity.
    } else {
      auto* instPtr = &inst;
      inst.osdAnimId = inst.animations.animateSpatial(
          inst.osdProgress, osdTarget,
          {.damping = Tokens::springSnap.damping, .stiffness = Tokens::springSnap.stiffness},
          [instPtr](float v) {
            instPtr->osdProgress = v;
            instPtr->osdBar->setProgress(std::clamp(v, 0.0F, 1.0F));
            instPtr->surface->requestRedraw();
          },
          [instPtr] { instPtr->osdAnimId = 0; }, &inst
      );
    }
  }
  inst.faceWidth[static_cast<std::size_t>(Face::Osd)] = static_cast<float>(kOsdWidth);
  inst.faceHeight[static_cast<std::size_t>(Face::Osd)] = kMinCardHeight;

  // Banner face.
  if (m_banner.has_value()) {
    inst.bannerTitle->setText(m_banner->title);
    inst.bannerBody->setText(m_banner->body);
    inst.bannerBody->setVisible(!m_banner->body.empty());
    inst.bannerApp->setText(m_banner->appName);
    inst.bannerTime->setText(m_banner->time);
  }
  if (m_banner.has_value()) {
    const BannerItem& b = *m_banner;
    const std::string key = std::to_string(b.id) + "|" + b.icon + "|" + b.desktopEntry + (b.imageData.has_value() ? "|img" : "");
    if (inst.bannerIconKey != key) {
      bool ok = false;
      bool pending = false;
      const auto& img = b.imageData;
      if (img.has_value() && img->width > 0 && img->height > 0 && !img->data.empty() && img->bitsPerSample == 8
          && ((img->channels == 4 && img->hasAlpha) || (img->channels == 3 && !img->hasAlpha))) {
        ok = inst.bannerImage->setSourceRaw(
            renderer, img->data.data(), img->data.size(), img->width, img->height, img->rowStride,
            img->channels == 3 ? PixmapFormat::RGB : PixmapFormat::RGBA, true
        );
      }
      for (const std::string* name : {&b.icon, &b.desktopEntry}) {
        if (ok || name->empty()) {
          continue;
        }
        std::string path;
        if (uri::isRemoteUrl(*name)) {
          // Same download cache as the media art; the island refreshes when the file lands.
          path = mpris::resolveArtworkSource(m_http, m_pendingArt, *name, refresher(), std::weak_ptr<void>(m_alive));
          pending = pending || path.empty();
        } else {
          path = uri::normalizeFileUrl(*name);
          if (path.empty() || path.front() != '/') {
            path = m_iconResolver.resolve(path, static_cast<int>(kBannerGlyph));
          }
        }
        ok = !path.empty() && inst.bannerImage->setSourceFile(renderer, path, static_cast<int>(kBannerGlyph));
      }
      inst.bannerImage->setVisible(ok);
      inst.bannerCircle->setVisible(!ok);
      if (ok || !pending) {
        inst.bannerIconKey = key;
      }
    }
  }
  {
    const float fixed = 2.0F * kBannerPad + kBannerGlyph + kHoverGap;
    for (Label* label : {inst.bannerTitle, inst.bannerBody}) {
      label->setMaxWidth(std::max(0.0F, inst.liveWidth - fixed));
      label->layout(renderer);
    }
    inst.bannerTime->layout(renderer);
    inst.bannerApp->setMaxWidth(std::max(0.0F, inst.liveWidth - fixed - inst.bannerTime->width() - kCaptionGap));
    inst.bannerApp->layout(renderer);
    const float header = inst.bannerApp->width() + kCaptionGap + inst.bannerTime->width();
    const float textW = std::max({inst.bannerTitle->width(), inst.bannerBody->visible() ? inst.bannerBody->width() : 0.0F, header});
    const float w = std::clamp(fixed + textW, kMinCardWidth, inst.liveWidth);
    inst.faceWidth[static_cast<std::size_t>(Face::Banner)] = w;
    inst.bannerTitle->setMaxWidth(w - fixed);
    inst.bannerBody->setMaxWidth(w - fixed);
    inst.bannerApp->setMaxWidth(w - fixed - inst.bannerTime->width() - kCaptionGap);
    const float body = inst.bannerBody->visible() ? std::min(2.0F * Tokens::typeFootnote.line, inst.bannerBody->height()) : 0.0F;
    inst.faceHeight[static_cast<std::size_t>(Face::Banner)] =
        ceil4(2.0F * kBannerPad + Tokens::typeCaption.line + Tokens::typeHeadline.line + body);
  }

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
  const float maxH = surfaceHeight(inst.scale);
  // Mirror on: the compositor's own clip curve; else a lerp on the shell's spring.
  const auto& ms = inst.mirror.shape;
  const bool follow = inst.mirror.active;
  const auto at = [&](std::size_t i, float from, float to) {
    return follow ? static_cast<float>(ms[i].value()) : lerp(from, to, sizeT);
  };
  const float cw = std::clamp(std::round(at(2, inst.from.w, inst.to.w)), 1.0F, maxW);
  const float ch = std::clamp(std::round(at(3, inst.from.h, inst.to.h)), 1.0F, maxH);
  const Rect cur{
      .x = std::clamp(std::round(at(0, inst.from.x, inst.to.x)), 0.0F, maxW - cw),
      .y = std::clamp(std::round(at(1, inst.from.y, inst.to.y)), 0.0F, maxH - ch),
      .w = cw,
      .h = ch,
      .radius = std::max(0.0F, at(4, inst.from.radius, inst.to.radius)),
  };
  inst.cur = cur;
  const Rect restR = faceRect(inst, Face::Rest);

  // With the compositor the fills span the surface and its clip morphs them; otherwise the fills are the island.
  const Rect box = inst.dsk != nullptr ? Rect{0.0F, 0.0F, inst.width, surfaceHeight(inst.scale), cur.radius} : cur;
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
  {
    // The style cuts the pill out at +offset, so the shadow node sits at the pill + offset.
    const auto offset = shadowDirectionOffset(m_config->config().shell.shadow.direction);
    inst.shadow->setPosition(cur.x + static_cast<float>(offset.x), cur.y + static_cast<float>(offset.y));
  }
  inst.shadow->setSize(cur.w, cur.h);
  inst.shadow->setRadius(cur.radius);
  // With the elevation property the compositor draws the one shadow along the clip; the shell box is the fallback.
  const bool compositorShadow = inst.dsk != nullptr && inst.dsk->hasElevationProperty();
  inst.shadow->setOpacity(compositorShadow ? 0.0F : restMix);

  for (std::size_t i = 0; i < kFaceCount; ++i) {
    const auto f = static_cast<Face>(i);
    Node* node = inst.faceNode[i];
    float opacity = 0.0F;
    float scale = 1.0F;
    if (f == inst.face) {
      // 7.4: the incoming face starts at 50 % of the travel and fades in over the next 30 %.
      opacity = std::clamp((t - 0.5F) / 0.3F, 0.0F, 1.0F);
      scale = kFaceScale + (1.0F - kFaceScale) * opacity;
    } else if (f == inst.prevFace) {
      // The outgoing face fades out at once (fade_out is about a quarter of the travel) and shrinks to 0.92.
      const float out = std::clamp(t * 4.0F, 0.0F, 1.0F);
      opacity = 1.0F - out;
      scale = 1.0F - (1.0F - kFaceScale) * out;
    }
    node->setVisible(opacity > 0.0F);
    node->setOpacity(opacity);
    node->setTransformOrigin(node->width() * 0.5F, node->height() * 0.5F);
    node->setScale(MotionService::instance().enabled() ? scale : 1.0F);
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
  const float shown = lerp(inst.space.restless ? 0.0F : inst.space.opacity, 1.0F, 1.0F - restMix);
  inst.sceneRoot->setOpacity(shown);
  if (compositorShadow && inst.face == Face::Rest && !inst.mirror.active && reveal >= 1.0F) {
    // At rest the pill follows the bar's auto-hide fade, and so does its shadow.
    const double level = kRestElevation * shown;
    if (std::abs(level - inst.dskLevel) > 0.004) {
      inst.dsk->set(DskSurfaceClient::Property::Elevation, {level, 0.0, 0.0, 0.0});
      inst.dskLevel = level;
    }
  }

  const float k = inst.scale;
  const auto ix = static_cast<int>(cur.x * k);
  const auto iy = static_cast<int>(cur.y * k);
  const auto iw = std::max(1, static_cast<int>(cur.w * k));
  const auto ih = std::max(1, static_cast<int>(cur.h * k));
  if (shown > 0.01F) {
    inst.surface->setBlurRegion(Surface::tessellateRoundedRect(ix, iy, iw, ih, cur.radius * k));
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
  auto* instPtr = &inst;
  if (mirrored(inst)) {
    // One clock: shape, shadow and fade all start at the compositor's `started` frame (syncDsk).
    const bool grow = inst.face != Face::Rest;
    Instance::Mirror& m = inst.mirror;
    m.fade = desk::motion::Spring(0.0, 1.0, revealSpring(grow));
    m.fadePending = true;
    m.fadeSec = 0.0;
    const std::uint32_t generation = m.generation;
    syncDsk(inst, true);
    if (m.generation == generation) {
      // The rect did not change: no compositor animation, so no `started`; the fade starts now.
      startMirror(inst, inst.to, grow, false);
      m.fadePending = false;
      m.fadeStartMs = m.startMs;
    }
    ensureMirrorDriver(inst);
    inst.surface->requestRedraw();
    return;
  }
  syncDsk(inst, true);
  if (!MotionService::instance().enabled()) {
    finishReveal(inst);
    applyReveal(inst, 1.0F);
    inst.surface->requestRedraw();
    return;
  }
  const auto setter = [this, instPtr](float value) { applyReveal(*instPtr, value); };
  const auto done = [this, instPtr] { finishReveal(*instPtr); };
  inst.revealAnimId = inst.animations.animateSpring(
      0.0F, 1.0F, revealSpring(inst.face != Face::Rest), setter, done, &inst, velocity
  );
  inst.surface->requestRedraw();
}

// Banner swipe: the island follows the pointer upward 1:1 and downward at 0.3; a release past 40 % of the card or a fast
// upward flick dismisses it on the collapse spring with the release velocity, anything else springs back.
void LiveActivityIsland::bannerDragStart(Instance& inst, float y) {
  if (inst.face != Face::Banner || inst.dsk == nullptr || !MotionService::instance().enabled()) {
    return;
  }
  if (!m_banner.has_value()) {
    return;
  }
  inst.dragTimer.stop();
  inst.dragBannerId = m_banner->id;
  inst.dragging = true;
  inst.dragMoved = false;
  inst.dragStartY = y;
  inst.dragLastY = inst.dragStartY;
  inst.dragLastAt = std::chrono::steady_clock::now();
  inst.dragVelocity = 0.0F;
}

float LiveActivityIsland::bannerMinOffset(const Instance& inst) const {
  const Rect banner = faceRect(inst, Face::Banner);
  return -(banner.y + banner.h - 1.0F);
}

void LiveActivityIsland::applyBannerOffset(Instance& inst, float offset) {
  inst.dragOffset = offset;
  inst.dsk->set(DskSurfaceClient::Property::Translate, {0.0, static_cast<double>(offset), 0.0, 0.0});
  inst.surface->requestRedraw();
}

bool LiveActivityIsland::bannerDragStale(const Instance& inst) const {
  return inst.face != Face::Banner || !m_banner.has_value() || m_banner->id != inst.dragBannerId;
}

void LiveActivityIsland::bannerDragMove(Instance& inst, float y) {
  if (!inst.dragging) {
    return;
  }
  if (bannerDragStale(inst)) {
    inst.dragging = false;
    startBannerSpring(inst, false);
    return;
  }
  const float screenY = y; // the pointer reports output-relative y; the moving surface does not shift it
  const auto now = std::chrono::steady_clock::now();
  const float dtMs = std::chrono::duration<float, std::milli>(now - inst.dragLastAt).count();
  // Samples closer than 4 ms are merged into the next one: they would give a wild speed.
  if (dtMs >= 4.0F) {
    inst.dragVelocity = dtMs < 80.0F ? std::clamp((screenY - inst.dragLastY) / dtMs, -4.0F, 4.0F) : 0.0F;
    inst.dragLastY = screenY;
    inst.dragLastAt = now;
  }
  const float delta = screenY - inst.dragStartY;
  if (std::abs(delta) > 4.0F) {
    inst.dragMoved = true;
  }
  if (inst.dragMoved) {
    applyBannerOffset(inst, std::max(delta < 0.0F ? delta : delta * 0.3F, bannerMinOffset(inst)));
  }
}

void LiveActivityIsland::bannerDragEnd(Instance& inst) {
  if (!inst.dragging) {
    return;
  }
  inst.dragging = false;
  if (!inst.dragMoved) {
    return;
  }
  if (std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - inst.dragLastAt).count() > 80.0F) {
    inst.dragVelocity = 0.0F;
  }
  const Rect banner = faceRect(inst, Face::Banner);
  const bool dismiss =
      !bannerDragStale(inst) && (inst.dragOffset < -0.4F * banner.h || inst.dragVelocity < -0.5F);
  startBannerSpring(inst, dismiss);
}

void LiveActivityIsland::startBannerSpring(Instance& inst, bool dismiss) {
  // The card keeps its bottom row on the output: a surface with nothing visible gets no frame callbacks, and its
  // client would never draw again.
  const float target = dismiss ? bannerMinOffset(inst) : 0.0F;
  auto* instPtr = &inst;
  const Tokens::Spring& token = dismiss ? Tokens::springCollapse : Tokens::springMorph;
  inst.dragDismiss = dismiss;
  // A timer steps the spring: a card swiped off the output gets no frame callbacks, so the frame clock would stall.
  inst.dragSpring = desk::motion::Spring(
      inst.dragOffset, target, {.damping = token.damping, .stiffness = token.stiffness}, inst.dragVelocity * 1000.0F
  );
  inst.dragLastAt = std::chrono::steady_clock::now();
  inst.dragTimer.startRepeating(std::chrono::milliseconds(8), [this, instPtr] {
    if (instPtr->dsk == nullptr) {
      instPtr->dragTimer.stop();
      return;
    }
    if (instPtr->dragDismiss && bannerDragStale(*instPtr)) {
      const Tokens::Spring& back = Tokens::springMorph;
      instPtr->dragDismiss = false;
      instPtr->dragSpring = desk::motion::Spring(
          instPtr->dragSpring.value(), 0.0, {.damping = back.damping, .stiffness = back.stiffness},
          instPtr->dragSpring.velocity()
      );
    }
    const auto now = std::chrono::steady_clock::now();
    const double dt = std::chrono::duration<double>(now - instPtr->dragLastAt).count();
    instPtr->dragLastAt = now;
    applyBannerOffset(*instPtr, static_cast<float>(instPtr->dragSpring.step(dt)));
    if (!instPtr->dragSpring.settled()) {
      return;
    }
    // stop() frees this closure: keep what follows out of its captures.
    LiveActivityIsland* const self = this;
    Instance* const inst = instPtr;
    const bool dismissed = inst->dragDismiss;
    inst->dragTimer.stop();
    self->finishBannerDrag(*inst, dismissed);
  });
}

void LiveActivityIsland::finishBannerDrag(Instance& inst, bool dismiss) {
  auto* instPtr = &inst;
  if (dismiss && m_banner.has_value() && m_banner->id == inst.dragBannerId) {
    endBanner();
    // The card is off the output: settle the new face at once, then bring the surface back.
    if (instPtr->revealAnimId != 0) {
      instPtr->animations.cancel(instPtr->revealAnimId);
      instPtr->revealAnimId = 0;
    }
    instPtr->reveal = 1.0F;
    instPtr->mirror.active = false;
    instPtr->from = instPtr->to;
    instPtr->prevFace = instPtr->face;
    instPtr->dskTarget.reset();
    syncDsk(*instPtr, false);
    applyReveal(*instPtr, 1.0F);
  }
  if (instPtr->dsk != nullptr) {
    applyBannerOffset(*instPtr, 0.0F);
  }
}

std::string LiveActivityIsland::describeActivity() const {
  if (!m_activity.has_value()) {
    return "none";
  }
  // Fields are joined by "|"; a "|" inside a field is written "\\|" and a line break becomes a space.
  const auto field = [](std::string text) {
    std::string out;
    for (const char c : text) {
      if (c == '|') {
        out += "\\|";
      } else {
        out += (c == '\n' || c == '\r') ? ' ' : c;
      }
    }
    return out;
  };
  return field(m_activity->title) + "|" + field(m_activity->subtitle) + "|" + field(m_activity->secondaryGlyph) + "|"
      + field(m_activity->actionGlyph);
}

bool LiveActivityIsland::pressActivityButton(bool secondary) {
  if (!m_activity.has_value()) {
    return false;
  }
  const auto& action = secondary ? m_activity->secondaryAction : m_activity->action;
  if (!action) {
    return false;
  }
  action();
  return true;
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

void LiveActivityIsland::syncHoverProgress(Instance& inst) {
  const MprisService* mpris = m_mpris ? m_mpris() : nullptr;
  const auto player = mpris != nullptr ? mpris->activePlayer() : std::nullopt;
  const auto position = mpris != nullptr ? mpris->positionActive() : std::nullopt;
  const bool known = player.has_value() && player->lengthUs > 0 && position.has_value();
  inst.hoverProgress->setProgress(known ? std::clamp(static_cast<float>(*position) / static_cast<float>(player->lengthUs), 0.0F, 1.0F) : 0.0F);
}

void LiveActivityIsland::tickBars() {
  m_barsPhase += 0.033F;
  for (auto& inst : m_instances) {
    if (inst->face == Face::HoverMedia && inst->hoverProgress != nullptr) {
      syncHoverProgress(*inst);
    }
  }
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
    const auto x = static_cast<float>(event.sx) / inst->scale;
    const auto y = static_cast<float>(event.sy) / inst->scale;
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
