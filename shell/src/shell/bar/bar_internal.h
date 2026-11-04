#pragma once

#include "shell/bar/bar_instance.h"

#include "compositors/compositor_platform.h"
#include "config/config_service.h"
#include "wayland/wayland_connection.h"

#include <array>
#include <cmath>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <wayland-client-protocol.h>

namespace bar_detail {

  // Hover highlight: peak fill alpha of the widget-foreground tint, and the cross-axis inset
  // (logical px, content-scaled) of per-member pills inside capsule groups.
  constexpr float kWidgetHoverFillAlpha = 0.1F;
  constexpr float kGroupHoverCrossInset = 2.0F;

  [[nodiscard]] inline bool barConfigUsesSlideSurface(const BarConfig& cfg) noexcept { return cfg.isAutoHideEnabled(); }

  [[nodiscard]] inline bool barSupportsSlideBehavior(const BarConfig& cfg) noexcept { return cfg.isAutoHideEnabled(); }

  [[nodiscard]] inline bool barPointerHideAllowed(const BarInstance& instance) noexcept {
    if (instance.barConfig.smartAutoHide) {
      return !instance.smartAutoHidePinnedVisible;
    }
    return instance.barConfig.autoHide;
  }

  [[nodiscard]] inline int barAutoHideEdgeGutter(const BarConfig& cfg) noexcept {
    if (!barConfigUsesSlideSurface(cfg) || cfg.marginEdge <= 0) {
      return 0;
    }
    return cfg.marginEdge;
  }

  struct BarSurfaceSpec {
    std::int32_t marginTop = 0;
    std::int32_t marginRight = 0;
    std::int32_t marginBottom = 0;
    std::int32_t marginLeft = 0;
    std::uint32_t surfaceWidth = 0;
    std::uint32_t surfaceHeight = 0;
    std::int32_t exclusiveZone = 0;
  };

  struct BarVisualGeometry {
    float x = 0.0F;
    float y = 0.0F;
    float width = 0.0F;
    float height = 0.0F;
  };

  BarVisualGeometry computeBarVisualGeometry(
      const BarConfig& cfg, const ShellConfig::ShadowConfig& shadow, float surfaceWidth, float surfaceHeight,
      float innerSurfaceExtension = 0.0F
  );
  [[nodiscard]] std::string activeWorkspaceId(const std::vector<Workspace>& workspaces);
  [[nodiscard]] bool smartAutoHideWantsPinnedVisible(const CompositorPlatform& platform, wl_output* output);
  bool pointInsideNode(const Node* node, float sceneX, float sceneY);
  void applyBarWidgetHitTargets(Node* node, const Node* slot, bool isVertical);
  Widget* widgetAtPoint(const std::vector<std::unique_ptr<Widget>>& widgets, float sceneX, float sceneY);
  Widget* widgetAtPoint(const BarInstance& instance, float sceneX, float sceneY);
  std::pair<float, float> surfaceOriginForOutputLocal(const BarInstance& instance, const WaylandOutput& outputInfo);
  void suppressTooltipForOpenPanel(std::string_view panelId);
  std::optional<std::array<float, 4>> islandRectAt(const BarInstance& instance, float sx, float sy);
  [[nodiscard]] BarSurfaceSpec
  computeBarSurfaceSpec(const BarConfig& barConfig, const ShellConfig::ShadowConfig& shadowConfig);
  [[nodiscard]] float barInnerSurfaceExtension(
      const BarConfig& cfg, const ShellConfig::ShadowConfig& shadow, float surfaceWidth, float surfaceHeight
  );
  void applyBarShadowStyle(
      BarInstance& instance, const ShellConfig::ShadowConfig& shadowConfig, float surfaceWidth, float surfaceHeight
  );
  float barMainPadding(const BarConfig& config);
  float barMainPaddingEnd(const BarConfig& config);
  void layoutBarIslands(BarInstance& instance, bool isVertical);
  InputArea* tooltipAreaForHover(BarInstance& instance, InputArea* hoveredArea);

} // namespace bar_detail
