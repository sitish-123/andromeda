#include "shell/live_activity/live_activity_island.h"

#include "config/config_service.h"
#include "config/config_types.h"
#include "core/deferred_call.h"
#include "core/log.h"
#include "core/ui_phase.h"
#include "render/render_context.h"
#include "render/scene/node.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "wayland/surface.h"
#include "wayland/wayland_connection.h"

#include <algorithm>
#include <cmath>

namespace {

  constexpr Logger kLog("live-activity");

  constexpr float kWidth = 440.0F;
  constexpr float kHeight = 52.0F;
  // Below this logical output width the island drops its progress bar, so it clears the bar's end island.
  constexpr std::int32_t kWideOutput = 1440;
  constexpr float kCompactWidth = 290.0F;
  // Collapsed size: the bar's center island, which the live island grows out of.
  constexpr float kCollapsedWidth = 120.0F;
  constexpr float kCollapsedHeight = 34.0F;
  constexpr float kPadding = 8.0F;
  constexpr float kIcon = 36.0F;
  constexpr float kProgressWidth = 150.0F;
  constexpr float kProgressHeight = 6.0F;
  // Width left for text: 440 - 2 x 8 padding - 36 icon - 150 bar - 36 button - 3 x 12 gaps.
  constexpr float kTitleMaxWidth = 166.0F;
  constexpr float kMaterialOpacity = 0.7F;
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
      .fill = colorSpecFromRole(ColorRole::SolidRegular, Style::glassAlpha(kMaterialOpacity)),
  });
  island->setClipChildren(true);

  auto title = ui::label({
      .out = &inst.title,
      .fontSize = Style::fontSizeBody,
      .fontWeight = FontWeight::Bold,
      .color = colorSpecFromRole(ColorRole::Label1),
      .maxWidth = kTitleMaxWidth,
      .maxLines = 1,
  });
  auto subtitle = ui::label({
      .out = &inst.subtitle,
      .fontSize = Style::fontSizeCaption,
      .color = colorSpecFromRole(ColorRole::Label2),
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
          .glyphSize = Style::fontSizeTitle,
          .color = colorSpecFromRole(ColorRole::OnAccent),
      })
  );

  auto content = ui::row(
      {
          .align = FlexAlign::Center,
          .gap = Style::spaceMd,
          .paddingH = kPadding,
          .width = inst.width,
          .height = kHeight,
      },
      std::move(iconCircle),
      ui::column({.align = FlexAlign::Start, .gap = 1.0F, .flexGrow = 1.0F}, std::move(title), std::move(subtitle)),
      ui::progressBar({
          .out = &inst.progress,
          .fill = colorSpecFromRole(ColorRole::Label1),
          .track = colorSpecFromRole(ColorRole::Fill2),
          .radius = kProgressHeight * 0.5F,
          .width = kProgressWidth,
          .height = kProgressHeight,
      }),
      ui::button({
          .out = &inst.action,
          .glyph = "close",
          .variant = ButtonVariant::Default,
          .radius = kIcon * 0.5F,
          .width = kIcon,
          .height = kIcon,
          .onClick =
              [this] {
                if (m_activity.has_value() && m_activity->action) {
                  m_activity->action();
                }
              },
      })
  );
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
  if (!a.actionGlyph.empty()) {
    inst.action->setGlyph(a.actionGlyph);
  }
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
