#include "shell/live_activity/live_activity_island.h"

#include "config/config_service.h"
#include "config/config_types.h"
#include "core/deferred_call.h"
#include "core/log.h"
#include "core/ui_phase.h"
#include "dbus/mpris/mpris_service.h"
#include "notification/notification_manager.h"
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

#include <algorithm>
#include <array>
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
  desk::motion::SpringConfig revealSpring() {
    return {.damping = Tokens::springStandard.damping, .stiffness = Tokens::springStandard.stiffness};
  }
  TOKENS_LIVE(kLiveRadius, Tokens::componentLiveRadius);
  TOKENS_LIVE(kDot, Tokens::componentIslandDot);
  TOKENS_LIVE(kRestWidth, Tokens::componentIslandCenterRestWidth);
  constexpr float kArt = 22.0F;
  constexpr float kArtRadius = 6.0F;
  constexpr float kRestPadding = 12.0F;

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
  }
  if (had != m_activity.has_value()) {
    for (auto& inst : m_instances) {
      animateReveal(*inst, m_activity.has_value() ? 1.0F : 0.0F);
    }
  }
  for (auto& inst : m_instances) {
    inst->surface->requestLayout();
  }
}

void LiveActivityIsland::onTimeTick() {
  if (m_date != dateText()) {
    refresh();
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
          static_cast<std::uint32_t>(kHeight)
      );
      inst->dskTarget.reset();
    }
    if (space->edgeMargin != inst->space.edgeMargin) {
      inst->surface->setMargins(space->bottom ? 0 : space->edgeMargin, 0, space->bottom ? space->edgeMargin : 0, 0);
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
            .height = static_cast<std::uint32_t>(kHeight),
            // -1: placed from the screen edge, over the bar's exclusive zone.
            .exclusiveZone = -1,
            .marginTop = space->bottom ? 0 : space->edgeMargin,
            .marginBottom = space->bottom ? space->edgeMargin : 0,
            .keyboard = LayerShellKeyboard::None,
            .defaultWidth = static_cast<std::uint32_t>(inst->width),
            .defaultHeight = static_cast<std::uint32_t>(kHeight),
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
    syncDsk(*inst, false);
    if (m_activity.has_value()) {
      animateReveal(*inst, 1.0F);
    }
    m_instances.push_back(std::move(inst));
  }
  syncVisibility();
}

void LiveActivityIsland::destroySurfaces() {
  for (auto& inst : m_instances) {
    inst->animations.cancelAll();
  }
  m_instances.clear();
}

LiveActivityIsland::Rect LiveActivityIsland::restRect(const Instance& inst) const {
  const float h = std::min(inst.space.height, static_cast<float>(kHeight));
  return Rect{
      .x = std::round((inst.width - inst.restWidth) * 0.5F),
      .y = inst.space.bottom ? kHeight - h : 0.0F,
      .w = inst.restWidth,
      .h = h,
      .radius = h * 0.5F,
  };
}

LiveActivityIsland::Rect LiveActivityIsland::liveRect(const Instance& inst) const {
  return Rect{
      .x = std::round((inst.width - inst.liveWidth) * 0.5F),
      .y = 0.0F,
      .w = inst.liveWidth,
      .h = kHeight,
      .radius = kLiveRadius,
  };
}

void LiveActivityIsland::syncDsk(Instance& inst, bool animate) {
  if (inst.dsk == nullptr) {
    return;
  }
  const bool live = inst.target > 0.5F;
  const Rect r = live ? liveRect(inst) : restRect(inst);
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
    inst.dsk->animate(DskSurfaceClient::Property::Clip, DskSurfaceClient::Curve::SpringStandard, clip);
    inst.dsk->animate(DskSurfaceClient::Property::Radius, DskSurfaceClient::Curve::SpringStandard, radius);
  }
  if (live) {
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
  const auto bar = [](float h) {
    return ui::box({.fill = colorSpecFromRole(ColorRole::Accent), .radius = 1.5F, .width = 3.0F, .height = h});
  };
  auto mediaGroup = ui::row(
      {.align = FlexAlign::Center, .gap = Style::spaceSm, .visible = false},
      ui::row(
          {.align = FlexAlign::Center,
           .justify = FlexJustify::Center,
           .fill = colorSpecFromRole(ColorRole::Label3, 0.25F),
           .radius = kArtRadius,
           .width = kArt,
           .height = kArt},
          ui::glyph({.glyph = "music", .glyphSize = 14.0F, .color = colorSpecFromRole(ColorRole::Label2)})
      ),
      ui::row({.align = FlexAlign::Center, .gap = 2.0F, .height = kArt}, bar(8.0F), bar(14.0F), bar(10.0F))
  );
  inst.mediaGroup = mediaGroup.get();
  auto restContent = ui::row(
      {.align = FlexAlign::Center, .gap = Style::spaceSm},
      std::move(mediaGroup),
      ui::label({
          .out = &inst.date,
          .fontSize = Tokens::typeCaption.size,
          .color = colorSpecFromRole(ColorRole::Label1),
          .maxLines = 1,
      }),
      ui::box({
          .out = &inst.divider,
          .fill = colorSpecFromRole(ColorRole::Stroke),
          .width = 1.0F,
          .height = 14.0F,
          .visible = false,
      }),
      dot(&inst.micDot, colorSpecFromRole(ColorRole::Accent)),
      dot(&inst.cameraDot, colorSpecFromRole(ColorRole::Accent)),
      dot(&inst.screenDot, colorSpecFromRole(ColorRole::Danger)),
      ui::label({
          .out = &inst.count,
          .fontSize = Tokens::typeCaption.size,
          .color = colorSpecFromRole(ColorRole::Label1),
          .maxLines = 1,
          .visible = false,
      })
  );
  inst.count->setLineHeight(Tokens::typeCaption.size * Tokens::typeCaption.lineHeight);
  inst.date->setLineHeight(Tokens::typeCaption.size * Tokens::typeCaption.lineHeight);
  inst.restContent = restContent.get();
  material->addChild(std::move(restContent));

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

  auto iconCircle = ui::row(
      {
          .align = FlexAlign::Center,
          .justify = FlexJustify::Center,
          .fill = colorSpecFromRole(ColorRole::Accent),
          .radius = kIcon * 0.5F,
          .width = kIcon,
          .height = kIcon,
      },
      ui::glyph({
          .out = &inst.icon,
          .glyphSize = Tokens::componentIslandIcon,
          .color = colorSpecFromRole(ColorRole::OnAccent),
      })
  );

  auto content = ui::row(
      {
          .align = FlexAlign::Center,
          .gap = Tokens::componentLiveGap,
          .paddingH = kPadding,
          .width = inst.width,
          .height = kHeight,
      },
      std::move(iconCircle),
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
  inst.content = content.get();
  tint->addChild(std::move(content));

  auto ring = ui::box({.out = &inst.ring, .fill = clearColorSpec()});
  auto* instPtr = &inst;
  auto restArea = ui::inputArea({
      .onClick =
          [this, instPtr](const InputArea::PointerData&) {
            if (m_open) {
              m_open(instPtr->output);
            }
          },
  });
  inst.restArea = restArea.get();

  inst.sceneRoot->addChild(std::move(material));
  inst.sceneRoot->addChild(std::move(tint));
  inst.sceneRoot->addChild(std::move(ring));
  inst.sceneRoot->addChild(std::move(restArea));
}

void LiveActivityIsland::applyContent(Instance& inst) {
  Renderer& renderer = inst.surface->renderTarget().renderer();

  inst.material->setFill(colorSpecFromRole(materialRole(), Style::glassAlpha(inst.space.backgroundOpacity)));
  inst.ring->setBorder(inst.space.border, inst.space.borderWidth);

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
  inst.restContent->layout(renderer);
  inst.restWidth = std::min(inst.space.width, std::max(static_cast<float>(kRestWidth), inst.restContent->width() + 2.0F * kRestPadding));
  inst.liveWidth = std::min(inst.width, std::max(kCollapsedWidth, inst.space.width));
  syncDsk(inst, true);

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
  inst.content->setSize(inst.liveWidth, kHeight);
  inst.content->layout(renderer);
}

void LiveActivityIsland::applyReveal(Instance& inst, float reveal) {
  inst.reveal = reveal;
  if (inst.material == nullptr || inst.sceneRoot == nullptr) {
    return;
  }
  const float t = std::clamp(reveal, 0.0F, 1.0F);
  const float sizeT = MotionService::instance().enabled() ? t : inst.target;
  const Rect from = restRect(inst);
  const Rect to = liveRect(inst);
  const Rect cur{
      .x = std::round(lerp(from.x, to.x, sizeT)),
      .y = std::round(lerp(from.y, to.y, sizeT)),
      .w = std::round(lerp(from.w, to.w, sizeT)),
      .h = std::round(lerp(from.h, to.h, sizeT)),
      .radius = lerp(from.radius, to.radius, sizeT),
  };

  // With the compositor the fills span the surface and its clip morphs them; otherwise the fills are the island.
  const Rect box = inst.dsk != nullptr ? Rect{0.0F, 0.0F, inst.width, kHeight, cur.radius} : cur;
  for (Box* fill : {inst.material, inst.tint}) {
    fill->setPosition(box.x, box.y);
    fill->setSize(box.w, box.h);
    fill->setRadius(box.radius);
  }
  inst.material->setOpacity(1.0F - t);
  inst.tint->setOpacity(t);
  inst.ring->setPosition(cur.x, cur.y);
  inst.ring->setSize(cur.w, cur.h);
  inst.ring->setRadius(cur.radius);
  inst.ring->setOpacity(1.0F - t);

  inst.restContent->setPosition(
      std::round(from.x + (from.w - inst.restContent->width()) * 0.5F - box.x),
      std::round(from.y + (from.h - inst.restContent->height()) * 0.5F - box.y)
  );
  inst.restContent->setOpacity(std::clamp(1.0F - t * 2.5F, 0.0F, 1.0F));
  inst.content->setPosition(to.x - box.x, to.y - box.y);
  inst.content->setOpacity(std::clamp((t - 0.4F) / 0.6F, 0.0F, 1.0F));

  inst.restArea->setPosition(from.x, from.y);
  inst.restArea->setSize(from.w, from.h);

  // The pill follows the bar's auto-hide fade; a live card stays.
  const float shown = lerp(inst.space.opacity, 1.0F, t);
  inst.sceneRoot->setOpacity(shown);
  inst.restArea->setHitTestVisible(inst.target < 0.5F && shown >= 0.5F);

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

void LiveActivityIsland::animateReveal(Instance& inst, float to) {
  if (inst.revealAnimId != 0) {
    inst.animations.cancel(inst.revealAnimId);
    inst.revealAnimId = 0;
  }
  inst.target = to;
  syncDsk(inst, true);
  auto* instPtr = &inst;
  const auto setter = [this, instPtr](float value) { applyReveal(*instPtr, value); };
  const auto done = [instPtr] { instPtr->revealAnimId = 0; };
  if (MotionService::instance().enabled()) {
    inst.revealAnimId = inst.animations.animateSpring(inst.reveal, to, revealSpring(), setter, done, &inst);
  } else {
    inst.revealAnimId = inst.animations.animateTimer(
        inst.reveal, to, static_cast<float>(Tokens::motionFast), Easing::Linear, setter, done, &inst
    );
  }
  inst.surface->requestRedraw();
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
