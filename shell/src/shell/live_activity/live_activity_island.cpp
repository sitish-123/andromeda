#include "shell/live_activity/live_activity_island.h"

#include "config/config_service.h"
#include "config/config_types.h"
#include "core/deferred_call.h"
#include "core/log.h"
#include "core/ui_phase.h"
#include "render/render_context.h"
#include "render/scene/node.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "wayland/dsk_surface_client.h"
#include "wayland/surface.h"
#include "wayland/wayland_connection.h"

#include <algorithm>
#include <cmath>

namespace {

  constexpr Logger kLog("live-activity");

  constexpr float kWidth = 440.0F;
  constexpr float kHeight = Tokens::componentLiveHeight;
  // Below this logical output width the island drops its progress bar, so it clears the bar's end island.
  constexpr std::int32_t kWideOutput = 1440;
  constexpr float kCompactWidth = 290.0F;
  // Collapsed size: the bar's center island, which the live island grows out of.
  constexpr float kCollapsedWidth = 120.0F;
  constexpr float kCollapsedHeight = Tokens::componentIslandHeight;
  constexpr float kPadding = Tokens::componentLivePadding;
  constexpr float kIcon = Tokens::componentLiveLead;
  constexpr float kButton = Tokens::componentLiveButton;
  constexpr float kProgressWidth = Tokens::componentLiveProgressWidth;
  constexpr float kProgressHeight = Tokens::componentLiveProgressHeight;
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
  constexpr float kFallbackTopMargin = 8.0F;
  constexpr desk::motion::SpringConfig kRevealSpring{.damping = 0.8, .stiffness = 260.0};

  float lerp(float a, float b, float t) { return a + (b - a) * t; }

} // namespace

LiveActivityIsland::LiveActivityIsland() = default;

LiveActivityIsland::~LiveActivityIsland() { destroySurfaces(); }

void LiveActivityIsland::initialize(WaylandConnection& wayland, ConfigService* config, RenderContext* renderContext) {
  m_wayland = &wayland;
  m_config = config;
  m_renderContext = renderContext;
}

void LiveActivityIsland::setVisibilityCallback(VisibilityCallback callback) {
  m_visibilityCallback = std::move(callback);
}

void LiveActivityIsland::setActivity(std::optional<LiveActivity> activity) {
  const bool had = m_activity.has_value();
  m_activity = std::move(activity);
  if (m_activity.has_value()) {
    m_shown = *m_activity;
    if (!had) {
      ensureSurfaces();
      notifyVisibility(true);
    }
    for (auto& inst : m_instances) {
      if (!had) {
        animateReveal(*inst, 1.0F);
      }
      inst->surface->requestUpdateOnly();
    }
    return;
  }
  if (had) {
    for (auto& inst : m_instances) {
      animateReveal(*inst, 0.0F);
    }
  }
}

void LiveActivityIsland::onOutputChange() {
  if (!m_activity.has_value()) {
    return;
  }
  ensureSurfaces();
}

std::int32_t LiveActivityIsland::topMargin() const {
  if (m_config != nullptr) {
    for (const auto& bar : m_config->config().bars) {
      if (bar.enabled && bar.position == "top") {
        return bar.marginEdge;
      }
    }
  }
  return static_cast<std::int32_t>(kFallbackTopMargin);
}

void LiveActivityIsland::ensureSurfaces() {
  if (m_wayland == nullptr || m_renderContext == nullptr) {
    return;
  }
  std::erase_if(m_instances, [this](const auto& inst) {
    return std::ranges::none_of(m_wayland->outputs(), [&inst](const auto& output) {
      return output.output == inst->output;
    });
  });

  for (const auto& output : m_wayland->outputs()) {
    if (!output.done || output.output == nullptr || !output.hasUsableGeometry()) {
      continue;
    }
    if (std::ranges::any_of(m_instances, [&output](const auto& inst) { return inst->output == output.output; })) {
      continue;
    }

    auto inst = std::make_unique<Instance>();
    inst->output = output.output;
    inst->width = output.logicalWidth >= kWideOutput ? kWidth : kCompactWidth;
    inst->surface = std::make_unique<LayerSurface>(
        *m_wayland,
        LayerSurfaceConfig{
            .nameSpace = "noctalia-live-activity",
            .layer = LayerShellLayer::Overlay,
            .anchor = LayerShellAnchor::Top,
            .width = static_cast<std::uint32_t>(inst->width),
            .height = static_cast<std::uint32_t>(kHeight),
            // -1: placed from the screen edge, over the bar's exclusive zone.
            .exclusiveZone = -1,
            .marginTop = topMargin(),
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
      kLog.warn("failed to create the live activity surface on {}", output.connectorName);
      continue;
    }
    inst->surface->setInputRegion({});
    if (auto* manager = m_wayland->dskSurfaceManager(); manager != nullptr) {
      inst->dsk = std::make_unique<DskSurfaceClient>(manager, inst->surface->wlSurface());
    }
    if (m_activity.has_value()) {
      animateReveal(*inst, 1.0F);
    }
    m_instances.push_back(std::move(inst));
  }
}

void LiveActivityIsland::destroySurfaces() {
  for (auto& inst : m_instances) {
    inst->animations.cancelAll();
  }
  m_instances.clear();
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

  auto island = ui::box({
      .out = &inst.island,
      .fill = fixedColorSpec(rgbaHex(Tokens::componentLiveTint)),
  });
  island->setClipChildren(true);

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
  island->addChild(std::move(content));
  inst.sceneRoot->addChild(std::move(island));
}

void LiveActivityIsland::applyContent(Instance& inst) {
  const LiveActivity& a = m_shown;
  inst.icon->setGlyph(a.glyph);
  inst.title->setText(a.title);
  inst.subtitle->setText(a.subtitle);
  inst.subtitle->setVisible(!a.subtitle.empty());
  inst.progress->setVisible(a.progress.has_value() && inst.width >= kWidth);
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
  const float textWidth = inst.width - 2.0F * kPadding - kIcon - static_cast<float>(gaps) * Tokens::componentLiveGap
      - (showBar ? kProgressWidth : 0.0F) - static_cast<float>(buttons) * kButton;
  inst.title->setMaxWidth(std::max(0.0F, textWidth));
  inst.subtitle->setMaxWidth(std::max(0.0F, textWidth));
  if (inst.sceneRoot != nullptr && inst.content != nullptr) {
    inst.content->layout(inst.surface->renderTarget().renderer());
  }
}

void LiveActivityIsland::applyReveal(Instance& inst, float reveal) {
  inst.reveal = reveal;
  if (inst.island == nullptr || inst.sceneRoot == nullptr) {
    return;
  }
  const float t = std::clamp(reveal, 0.0F, 1.0F);
  if (inst.dsk != nullptr) {
    // The compositor clips the full-size island; the shell only fades the content.
    const int fullW = static_cast<int>(inst.width);
    const int fullH = static_cast<int>(kHeight);
    inst.island->setPosition(0.0F, 0.0F);
    inst.island->setSize(inst.width, kHeight);
    inst.island->setRadius(kHeight * 0.5F);
    inst.content->setPosition(0.0F, 0.0F);
    inst.content->setOpacity(std::clamp((reveal - 0.4F) / 0.6F, 0.0F, 1.0F));
    inst.surface->setBlurRegion(Surface::tessellateRoundedRect(0, 0, fullW, fullH, kHeight * 0.5F));
    inst.surface->setInputRegion(t >= 1.0F ? std::vector<InputRect>{InputRect{0, 0, fullW, fullH}} : std::vector<InputRect>{});
    inst.surface->requestRedraw();
    return;
  }
  const float w = std::round(lerp(kCollapsedWidth, inst.width, t));
  const float h = std::round(lerp(kCollapsedHeight, kHeight, t));
  const float x = std::round((inst.sceneRoot->width() - w) * 0.5F);
  inst.island->setPosition(x, 0.0F);
  inst.island->setSize(w, h);
  inst.island->setRadius(h * 0.5F);
  inst.island->setOpacity(std::clamp(reveal * 2.0F, 0.0F, 1.0F));
  inst.content->setPosition(std::round((w - inst.width) * 0.5F), std::round((h - kHeight) * 0.5F));
  inst.content->setOpacity(std::clamp((reveal - 0.4F) / 0.6F, 0.0F, 1.0F));

  const int ix = static_cast<int>(x);
  const int iw = std::max(1, static_cast<int>(w));
  const int ih = std::max(1, static_cast<int>(h));
  inst.surface->setBlurRegion(Surface::tessellateRoundedRect(ix, 0, iw, ih, h * 0.5F));
  if (t >= 1.0F) {
    inst.surface->setInputRegion({InputRect{ix, 0, iw, ih}});
  } else {
    inst.surface->setInputRegion({});
  }
  inst.surface->requestRedraw();
}

void LiveActivityIsland::animateReveal(Instance& inst, float to) {
  if (inst.revealAnimId != 0) {
    inst.animations.cancel(inst.revealAnimId);
    inst.revealAnimId = 0;
  }
  if (inst.dsk != nullptr) {
    const double collapsedX = std::round((inst.width - kCollapsedWidth) * 0.5F);
    const DskSurfaceClient::Value collapsed{collapsedX, 0.0, kCollapsedWidth, kCollapsedHeight};
    const DskSurfaceClient::Value full{0.0, 0.0, inst.width, kHeight};
    if (inst.reveal <= 0.0F) {
      inst.dsk->set(DskSurfaceClient::Property::Clip, collapsed);
      inst.dsk->set(DskSurfaceClient::Property::Radius, {kCollapsedHeight * 0.5, 0.0, 0.0, 0.0});
    }
    const bool grow = to > 0.0F;
    inst.dsk->animate(DskSurfaceClient::Property::Clip, DskSurfaceClient::Curve::SpringStandard, grow ? full : collapsed);
    inst.dsk->animate(
        DskSurfaceClient::Property::Radius, DskSurfaceClient::Curve::SpringStandard,
        {(grow ? kHeight : kCollapsedHeight) * 0.5, 0.0, 0.0, 0.0}
    );
  }
  auto* instPtr = &inst;
  inst.revealAnimId = inst.animations.animateSpring(
      inst.reveal, to, kRevealSpring, [this, instPtr](float value) { applyReveal(*instPtr, value); },
      [this, instPtr, to] {
        instPtr->revealAnimId = 0;
        if (to <= 0.0F && !m_activity.has_value()) {
          // The last surface to finish shrinking removes them all.
          if (std::ranges::all_of(m_instances, [](const auto& other) { return other->revealAnimId == 0; })) {
            DeferredCall::callLater([this] {
              if (!m_activity.has_value()) {
                destroySurfaces();
                notifyVisibility(false);
              }
            });
          }
        }
      },
      &inst
  );
  inst.surface->requestRedraw();
}

void LiveActivityIsland::notifyVisibility(bool visible) {
  if (m_visible == visible) {
    return;
  }
  m_visible = visible;
  if (m_visibilityCallback) {
    m_visibilityCallback(visible);
  }
}
