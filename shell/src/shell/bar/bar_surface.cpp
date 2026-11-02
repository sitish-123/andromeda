#include "shell/bar/bar.h"

#include "shell/bar/bar_internal.h"
#include "compositors/compositor_platform.h"
#include "config/config_service.h"
#include "core/deferred_call.h"
#include "core/log.h"
#include "core/process/process.h"
#include "core/scoped_timer.h"
#include "core/timer_manager.h"
#include "core/ui_phase.h"
#include "idle/idle_inhibitor.h"
#include "ipc/ipc_arg_parse.h"
#include "ipc/ipc_service.h"
#include "render/render_context.h"
#include "render/scene/input_area.h"
#include "shell/bar/bar_corner_shape.h"
#include "shell/bar/bar_reserved_zone.h"
#include "shell/bar/widget.h"
#include "shell/bar/widget_gesture_defaults.h"
#include "shell/bar/widgets/plugin_widget.h"
#include "shell/bar/widgets/taskbar_widget.h"
#include "shell/bar/widgets/spacer_widget.h"
#include "shell/bar/widgets/tray_widget.h"
#include "shell/panel/panel_manager.h"
#include "wayland/dsk_surface_client.h"
#include "ui/surface_shadow.h"
#include "shell/tooltip/tooltip_manager.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/string_utils.h"
#include "wayland/wayland_connection.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <linux/input-event-codes.h>
#include <optional>
#include <ranges>
#include <unordered_set>
#include <wayland-client-core.h>
#include <wayland-client-protocol.h>

using namespace bar_detail;

namespace {

  constexpr float kAutoHideSlideExtraPx = 4.0F;

  // Sizes a run's hover overlays after the capsule geometry is final. Single runs (and plain
  // widgets) get one overlay covering the whole shell; group runs get one pill per member so
  // only the hovered member lights up.
  void placeCapsuleHoverBoxes(
      BarCapsuleRun& run, bool isVertical, float shellW, float shellH, float contentX, float contentY,
      float capsuleRadius, float widgetHoverPadding
  ) {
    if (run.hoverBoxes.empty()) {
      return;
    }
    if (run.container == nullptr) {
      Box* box = run.hoverBoxes.front();
      if (box == nullptr) {
        return;
      }
      box->setPosition(0.0F, 0.0F);
      box->setSize(shellW, shellH);
      box->setRadius(capsuleRadius);
      return;
    }
    const float scale = run.contentScale;
    const float crossInset = kGroupHoverCrossInset * scale;
    // Use the normal per-widget inset, independent of the shared capsule's configurable
    // padding, so grouped and standalone widgets receive the same hover treatment.
    const float mainInset = widgetHoverPadding * scale;
    const float shellMain = isVertical ? shellH : shellW;
    const float shellCross = isVertical ? shellW : shellH;
    const float contentMain = isVertical ? contentY : contentX;
    const float crossExtent = std::max(0.0F, shellCross - 2.0F * crossInset);
    for (std::size_t i = 0; i < run.widgets.size() && i < run.hoverBoxes.size(); ++i) {
      // outerNode(), not root(): the outer gesture area is what the run container lays out, so it
      // carries the member's position; root() sits at (0,0) inside it.
      Node* member = run.widgets[i] != nullptr ? run.widgets[i]->outerNode() : nullptr;
      Box* box = run.hoverBoxes[i];
      if (member == nullptr || box == nullptr) {
        continue;
      }
      // Skip hidden members — Flex leaves them at stale (0,0) geometry.
      if (!member->visible() || !member->participatesInLayout()) {
        box->setSize(0.0F, 0.0F);
        continue;
      }
      const float rootStart = contentMain + (isVertical ? member->y() : member->x());
      const float rootExtent = isVertical ? member->height() : member->width();
      const float mainStart = std::max(0.0F, rootStart - mainInset);
      const float mainExtent = std::max(0.0F, std::min(shellMain, rootStart + rootExtent + mainInset) - mainStart);
      if (isVertical) {
        box->setPosition(crossInset, mainStart);
        box->setSize(crossExtent, mainExtent);
      } else {
        box->setPosition(mainStart, crossInset);
        box->setSize(mainExtent, crossExtent);
      }
      const float pillRadius = std::max(0.0F, std::min(box->width(), box->height()) * 0.5F);
      box->setRadius(std::min(pillRadius, std::max(0.0F, capsuleRadius - crossInset)));
    }
  }

  struct CapsuleMemberGeometry {
    std::size_t widgetIndex = 0;
    float x = 0.0F;
    float y = 0.0F;
    float width = 0.0F;
    float height = 0.0F;
    float sliceStart = 0.0F;
    float sliceEnd = 0.0F;

    [[nodiscard]] float mainStart(bool isVertical) const noexcept { return isVertical ? y : x; }
    [[nodiscard]] float mainEnd(bool isVertical) const noexcept {
      return mainStart(isVertical) + (isVertical ? height : width);
    }
  };

  [[nodiscard]] std::vector<CapsuleMemberGeometry>
  capsuleMemberSlices(const BarCapsuleRun& run, bool isVertical, float widgetHoverPadding) {
    std::vector<CapsuleMemberGeometry> members;
    if (run.shell == nullptr) {
      return members;
    }
    members.reserve(run.widgets.size());

    float shellX = 0.0F;
    float shellY = 0.0F;
    Node::absolutePosition(run.shell, shellX, shellY);
    for (std::size_t i = 0; i < run.widgets.size(); ++i) {
      Widget* widget = run.widgets[i];
      Node* root = widget != nullptr ? widget->outerNode() : nullptr;
      if (root == nullptr || !root->visible() || !root->participatesInLayout() || !root->hitTestVisible()) {
        continue;
      }

      float rootX = 0.0F;
      float rootY = 0.0F;
      Node::absolutePosition(root, rootX, rootY);
      members.push_back(
          CapsuleMemberGeometry{
              .widgetIndex = i,
              .x = rootX - shellX,
              .y = rootY - shellY,
              .width = root->width(),
              .height = root->height(),
          }
      );
    }

    const float shellMain = isVertical ? run.shell->height() : run.shell->width();
    for (std::size_t i = 0; i < members.size(); ++i) {
      auto& member = members[i];
      const float memberStart = member.mainStart(isVertical);
      const float memberEnd = member.mainEnd(isVertical);
      const float pad = widgetHoverPadding * run.widgets[member.widgetIndex]->contentScale();
      float before = pad;
      float after = pad;

      if (i > 0) {
        before = std::min(before, std::max(0.0F, (memberStart - members[i - 1].mainEnd(isVertical)) * 0.5F));
      }
      if (i + 1 < members.size()) {
        after = std::min(after, std::max(0.0F, (members[i + 1].mainStart(isVertical) - memberEnd) * 0.5F));
      }

      if (members.size() > 1) {
        if (i == 0) {
          before = after;
        }
        if (i + 1 == members.size()) {
          after = before;
        }
      }
      if (run.hasPaintedCapsuleBackground) {
        if (i == 0) {
          before = std::min(before, std::max(0.0F, memberStart));
        }
        if (i + 1 == members.size()) {
          after = std::min(after, std::max(0.0F, shellMain - memberEnd));
        }
      }
      member.sliceStart = memberStart - before;
      member.sliceEnd = memberEnd + after;
    }
    return members;
  }

  // Extends each member's hit target across the capsule padding and half the gap to its
  // neighbors, so hover/click coverage matches the capsule ink instead of stopping at the
  // content edge. Runs after applyBarWidgetHitTargets (which replaces outsets each layout).
  void extendCapsuleHitTargets(std::vector<BarCapsuleRun>& runs, bool isVertical, float widgetHoverPadding) {
    for (auto& run : runs) {
      if (run.shell == nullptr || run.hoverBoxes.empty()) {
        continue;
      }

      // Single runs (capsule or ghost pill): the hover overlay rect is the hit region.
      if (run.container == nullptr) {
        Widget* widget = !run.widgets.empty() ? run.widgets.front() : nullptr;
        Box* box = run.hoverBoxes.front();
        auto* area = widget != nullptr ? widget->outerNode() : nullptr;
        if (area == nullptr || box == nullptr || !area->visible() || !area->participatesInLayout()) {
          continue;
        }
        const float areaStart = isVertical ? area->y() : area->x();
        const float areaEnd = areaStart + (isVertical ? area->height() : area->width());
        const float boxStart = isVertical ? box->y() : box->x();
        const float boxEnd = boxStart + (isVertical ? box->height() : box->width());
        auto outset = area->hitTestOutset();
        if (isVertical) {
          outset.top += std::max(0.0F, areaStart - boxStart);
          outset.bottom += std::max(0.0F, boxEnd - areaEnd);
        } else {
          outset.left += std::max(0.0F, areaStart - boxStart);
          outset.right += std::max(0.0F, boxEnd - areaEnd);
        }
        area->setHitTestOutset(outset);
        continue;
      }

      // Tile only laid-out members; hidden ones keep stale geometry, and members clipped out of a
      // collapsed accordion are input-suppressed so their slice belongs to the visible member.
      const auto laidOut = capsuleMemberSlices(run, isVertical, widgetHoverPadding);
      const float shellMain = isVertical ? run.shell->height() : run.shell->width();
      float minSliceStart = 0.0F;
      float maxSliceEnd = shellMain;
      for (const auto& member : laidOut) {
        const std::size_t i = member.widgetIndex;
        Node* area = run.widgets[i]->outerNode();
        const float memberStart = member.mainStart(isVertical);
        const float memberEnd = member.mainEnd(isVertical);
        const float sliceStart = member.sliceStart;
        const float sliceEnd = member.sliceEnd;
        minSliceStart = std::min(minSliceStart, sliceStart);
        maxSliceEnd = std::max(maxSliceEnd, sliceEnd);

        auto outset = area->hitTestOutset();
        const float before = std::max(0.0F, memberStart - sliceStart);
        const float after = std::max(0.0F, sliceEnd - memberEnd);
        if (isVertical) {
          outset.top += before;
          outset.bottom += after;
        } else {
          outset.left += before;
          outset.right += after;
        }
        area->setHitTestOutset(outset);
      }
      auto shellOutset = run.shell->hitTestOutset();
      if (isVertical) {
        shellOutset.top += std::max(0.0F, -minSliceStart);
        shellOutset.bottom += std::max(0.0F, maxSliceEnd - shellMain);
      } else {
        shellOutset.left += std::max(0.0F, -minSliceStart);
        shellOutset.right += std::max(0.0F, maxSliceEnd - shellMain);
      }
      run.shell->setHitTestOutset(shellOutset);
    }
  }


} // namespace

namespace bar_detail {

  [[nodiscard]] BarSurfaceSpec
  computeBarSurfaceSpec(const BarConfig& barConfig, const ShellConfig::ShadowConfig& shadowConfig) {
    const bool vertical = (barConfig.position == "left" || barConfig.position == "right");
    const bool isBottom = barConfig.position == "bottom";
    const bool isRight = barConfig.position == "right";
    const std::int32_t mEnds =
        barConfig.islands ? static_cast<std::int32_t>(Tokens::spaceChromeGap) : barConfig.marginEnds;
    const std::int32_t mEdge = barConfig.marginEdge;
    const auto sb = ui::surface_shadow::bleed(barConfig.shadow, shadowConfig);
    const int edgeGutter = barAutoHideEdgeGutter(barConfig);
    const auto concave = barConcaveShape(barConfig);
    const int insetL = static_cast<int>(std::ceil(std::max(0.0F, concave.logicalInset.left)));
    const int insetT = static_cast<int>(std::ceil(std::max(0.0F, concave.logicalInset.top)));
    const int insetR = static_cast<int>(std::ceil(std::max(0.0F, concave.logicalInset.right)));
    const int insetB = static_cast<int>(std::ceil(std::max(0.0F, concave.logicalInset.bottom)));
    // Reserve room for a concave-corner spike on the inner edge (opaque bar material),
    // in addition to the shadow bleed that renders beyond the spike tips.
    const int concaveBulge = static_cast<int>(std::lround(concave.innerBulge));

    const std::int32_t edgeMargin = barEdgeLayerMargin(barConfig, shadowConfig);

    BarSurfaceSpec spec;
    if (!vertical) {
      spec.marginLeft = std::max(0, mEnds - sb.left - insetL);
      spec.marginRight = std::max(0, mEnds - sb.right - insetR);
      if (isBottom) {
        if (edgeGutter > 0) {
          // Surface reaches the screen edge (no layer margin); the margin is folded
          // into the surface as a gutter on the edge side. Do not add the edge-side
          // bleed here — it lives inside the gutter, not beyond it.
          spec.surfaceHeight = static_cast<std::uint32_t>(sb.up + concaveBulge + barConfig.thickness + edgeGutter);
        } else {
          spec.marginBottom = edgeMargin;
          spec.surfaceHeight =
              static_cast<std::uint32_t>(sb.up + concaveBulge + barConfig.thickness + std::min(mEdge, sb.down));
        }
      } else {
        if (edgeGutter > 0) {
          spec.surfaceHeight = static_cast<std::uint32_t>(sb.down + concaveBulge + barConfig.thickness + edgeGutter);
        } else {
          spec.marginTop = edgeMargin;
          spec.surfaceHeight =
              static_cast<std::uint32_t>(std::min(mEdge, sb.up) + barConfig.thickness + sb.down + concaveBulge);
        }
      }
    } else {
      spec.marginTop = std::max(0, mEnds - sb.up - insetT);
      spec.marginBottom = std::max(0, mEnds - sb.down - insetB);
      if (isRight) {
        if (edgeGutter > 0) {
          spec.surfaceWidth = static_cast<std::uint32_t>(sb.left + concaveBulge + barConfig.thickness + edgeGutter);
        } else {
          spec.marginRight = edgeMargin;
          spec.surfaceWidth =
              static_cast<std::uint32_t>(sb.left + concaveBulge + barConfig.thickness + std::min(mEdge, sb.right));
        }
      } else {
        if (edgeGutter > 0) {
          spec.surfaceWidth = static_cast<std::uint32_t>(sb.right + concaveBulge + barConfig.thickness + edgeGutter);
        } else {
          spec.marginLeft = edgeMargin;
          spec.surfaceWidth =
              static_cast<std::uint32_t>(std::min(mEdge, sb.left) + barConfig.thickness + sb.right + concaveBulge);
        }
      }
    }

    spec.exclusiveZone = barConfig.reserveSpace ? reservedBarExclusiveZone(barConfig, shadowConfig) : 0;
    return spec;
  }

  [[nodiscard]] float barInnerSurfaceExtension(
      const BarConfig& cfg, const ShellConfig::ShadowConfig& shadow, float surfaceWidth, float surfaceHeight
  ) {
    const auto base = computeBarSurfaceSpec(cfg, shadow);
    const bool isVertical = (cfg.position == "left" || cfg.position == "right");
    const float current = isVertical ? surfaceWidth : surfaceHeight;
    const auto normal = static_cast<float>(isVertical ? base.surfaceWidth : base.surfaceHeight);
    return std::max(0.0F, current - normal);
  }


  BarVisualGeometry computeBarVisualGeometry(
      const BarConfig& cfg, const ShellConfig::ShadowConfig& shadow, float surfaceWidth, float surfaceHeight,
      float innerSurfaceExtension
  ) {
    const auto barThickness = static_cast<float>(cfg.thickness);
    const float marginEnds = cfg.islands ? Tokens::spaceChromeGap : static_cast<float>(cfg.marginEnds);
    const auto marginEdge = static_cast<float>(cfg.marginEdge);
    const bool isBottom = cfg.position == "bottom";
    const bool isRight = cfg.position == "right";
    const bool isVertical = (cfg.position == "left" || cfg.position == "right");
    const auto sbi = ui::surface_shadow::bleed(cfg.shadow, shadow);
    const auto concave = barConcaveShape(cfg);
    const float insetL = std::ceil(std::max(0.0F, concave.logicalInset.left));
    const float insetT = std::ceil(std::max(0.0F, concave.logicalInset.top));
    const float insetR = std::ceil(std::max(0.0F, concave.logicalInset.right));
    const float insetB = std::ceil(std::max(0.0F, concave.logicalInset.bottom));
    const auto bleedLeft = static_cast<float>(sbi.left);
    const auto bleedRight = static_cast<float>(sbi.right);
    const auto bleedUp = static_cast<float>(sbi.up);
    const auto bleedDown = static_cast<float>(sbi.down);
    // For bottom/right bars the inner edge is the origin side, so the concave spike
    // pushes the body inward by its bulge. Top/left bars grow away from the origin
    // and need no body shift. Gutter (auto-hide) placements derive from the surface
    // size, which already includes the bulge, so they shift automatically.
    const float concaveBulge = concave.innerBulge;

    if (isVertical) {
      // Vertical bar: edge gap is left/right, ends inset is top/bottom.
      const float y = std::min(marginEnds, bleedUp) + insetT;
      float x = isRight ? (bleedLeft + concaveBulge) : std::min(marginEdge, bleedLeft);
      if (const int gutter = barAutoHideEdgeGutter(cfg); gutter > 0) {
        // The gutter equals marginEdge and sits between the screen edge and the bar.
        // Position the bar exactly marginEdge from the edge so it matches the
        // non-auto-hide placement; the edge-side shadow bleeds into the gutter.
        if (isRight) {
          x = surfaceWidth - static_cast<float>(gutter) - barThickness;
        } else {
          x = static_cast<float>(gutter);
        }
      } else if (isRight) {
        x += innerSurfaceExtension;
      }
      return {
          .x = x,
          .y = y,
          .width = barThickness,
          .height = surfaceHeight - y - std::min(marginEnds, bleedDown) - insetB,
      };
    }

    // Horizontal bar: edge gap is top/bottom, ends inset is left/right.
    const float x = std::min(marginEnds, bleedLeft) + insetL;
    float y = isBottom ? (bleedUp + concaveBulge) : std::min(marginEdge, bleedUp);
    if (const int gutter = barAutoHideEdgeGutter(cfg); gutter > 0) {
      if (isBottom) {
        y = surfaceHeight - static_cast<float>(gutter) - barThickness;
      } else {
        y = static_cast<float>(gutter);
      }
    } else if (isBottom) {
      y += innerSurfaceExtension;
    }
    return {
        .x = x,
        .y = y,
        .width = surfaceWidth - x - std::min(marginEnds, bleedRight) - insetR,
        .height = barThickness,
    };
  }


} // namespace

namespace {

  std::pair<float, float> computeAutoHideHiddenDelta(
      bool isVertical, bool isBottom, bool isRight, float w, float h, float contentLeft, float contentTop,
      float contentRight, float contentBottom
  ) {
    const float k = kAutoHideSlideExtraPx;
    if (!isVertical) {
      if (isBottom) {
        return {0.0F, (h - contentTop) + k};
      }
      return {0.0F, -(contentBottom + k)};
    }
    if (isRight) {
      return {(w - contentLeft) + k, 0.0F};
    }
    return {-(contentRight + k), 0.0F};
  }


} // namespace

namespace bar_detail {

  void applyBarShadowStyle(
      BarInstance& instance, const ShellConfig::ShadowConfig& shadowConfig, float surfaceWidth, float surfaceHeight
  ) {
    if (instance.shadow == nullptr) {
      return;
    }
    if (instance.barConfig.islands) {
      instance.shadow->setVisible(false);
      instance.islandShadowOffset = shadowDirectionOffset(shadowConfig.direction);
      instance.islandShadowStyle = ui::surface_shadow::style(
          shadowConfig, Style::glassAlpha(std::clamp(instance.barConfig.backgroundOpacity, 0.0F, 1.0F)),
          ui::surface_shadow::Shape{}
      );
      return;
    }

    const auto concave = barConcaveShape(instance.barConfig);
    const float innerSurfaceExtension =
        barInnerSurfaceExtension(instance.barConfig, shadowConfig, surfaceWidth, surfaceHeight);
    const auto barVisual =
        computeBarVisualGeometry(instance.barConfig, shadowConfig, surfaceWidth, surfaceHeight, innerSurfaceExtension);
    // Shadow follows the same shape as the background: the body expanded outward by
    // the concave inset into the visual rect, so concave spikes cast a matching shadow.
    const float barAreaW = barVisual.width + concave.logicalInset.left + concave.logicalInset.right;
    const float barAreaH = barVisual.height + concave.logicalInset.top + concave.logicalInset.bottom;
    const float bgOpacity = Style::glassAlpha(std::clamp(instance.barConfig.backgroundOpacity, 0.0F, 1.0F));
    const auto shadowOff = shadowDirectionOffset(shadowConfig.direction);
    const float shadowX = barVisual.x - concave.logicalInset.left + static_cast<float>(shadowOff.x);
    const float shadowY = barVisual.y - concave.logicalInset.top + static_cast<float>(shadowOff.y);
    RoundedRectStyle shadowStyle = ui::surface_shadow::style(
        shadowConfig, bgOpacity,
        ui::surface_shadow::Shape{
            .corners = concave.corners, .logicalInset = concave.logicalInset, .radius = concave.radii
        }
    );

    const bool panelShadowExclusion = instance.attachedPanelGeometry.has_value()
        && instance.attachedPanelGeometry->width > 0.0F
        && instance.attachedPanelGeometry->height > 0.0F;
    if (panelShadowExclusion) {
      const auto& attached = *instance.attachedPanelGeometry;
      const float convexRadius = std::max(0.0F, attached.cornerRadius);
      const float bulgeRadius = std::max(0.0F, attached.bulgeRadius);
      const std::string_view barPosition = instance.barConfig.position;
      const auto corners = attached_panel::cornerShapes(barPosition);
      const auto pickRadius = [&](CornerShape shape) {
        return shape == CornerShape::Concave ? bulgeRadius : convexRadius;
      };
      shadowStyle.shadowExclusion = true;
      shadowStyle.shadowExclusionOffsetX = shadowX - attached.x;
      shadowStyle.shadowExclusionOffsetY = shadowY - attached.y;
      shadowStyle.shadowExclusionWidth = attached.width;
      shadowStyle.shadowExclusionHeight = attached.height;
      shadowStyle.shadowExclusionCorners = corners;
      shadowStyle.shadowExclusionLogicalInset = attached_panel::logicalInset(barPosition, bulgeRadius);
      shadowStyle.shadowExclusionRadius =
          Radii{pickRadius(corners.tl), pickRadius(corners.tr), pickRadius(corners.br), pickRadius(corners.bl)};
    }

    auto configureShadow = [&](Box* node, float x, float y) {
      if (node == nullptr) {
        return;
      }
      node->setStyle(shadowStyle);
      node->setZIndex(-1);
      node->setPosition(x, y);
      node->setSize(barAreaW, barAreaH);
    };

    instance.shadow->setHitTestVisible(false);
    instance.shadow->setVisible(true);
    configureShadow(instance.shadow, shadowX, shadowY);

    if (instance.shadowLeftClip != nullptr) {
      instance.shadowLeftClip->setVisible(false);
    }
    if (instance.shadowRightClip != nullptr) {
      instance.shadowRightClip->setVisible(false);
    }
  }


} // namespace

namespace {

  void layoutBarSections(
      BarInstance& instance, Renderer& renderer, float barAreaW, float barAreaH, float padding, bool isVertical
  ) {
    const float slotCross = isVertical ? barAreaW : barAreaH;

    // Capsule cross-size is a fraction of the bar thickness (capsule_thickness), the same for every capsule
    // regardless of per-widget content scale. The max() guard keeps a thin bar from yielding a 0px capsule.
    const float capsuleCross = std::max(1.0F, std::round(slotCross * instance.barConfig.capsuleThickness));

    auto layoutWidgets = [&](std::vector<std::unique_ptr<Widget>>& widgets) {
      for (auto& widget : widgets) {
        if (auto* tray = dynamic_cast<TrayWidget*>(widget.get())) {
          tray->setCapsuleCross(capsuleCross);
        }
        if (widget->root() != nullptr) {
          widget->layout(renderer, barAreaW, barAreaH);
        }
      }
    };
    layoutWidgets(instance.startWidgets);
    layoutWidgets(instance.centerWidgets);
    layoutWidgets(instance.endWidgets);

    // An island has no neighbour to separate at its edges: a spacer with no visible widget on one side adds only an
    // empty strip, so it collapses.
    auto collapseEdgeSpacers = [](std::vector<std::unique_ptr<Widget>>& widgets) {
      const auto isContent = [](const std::unique_ptr<Widget>& w) {
        return dynamic_cast<SpacerWidget*>(w.get()) == nullptr && w->root() != nullptr && w->root()->visible();
      };
      const auto first = std::ranges::find_if(widgets, isContent);
      const auto last = std::ranges::find_if(widgets.rbegin(), widgets.rend(), isContent).base();
      for (auto it = widgets.begin(); it != widgets.end(); ++it) {
        if (dynamic_cast<SpacerWidget*>(it->get()) != nullptr && (*it)->root() != nullptr) {
          (*it)->root()->setVisible(first != widgets.end() && it > first && it < last);
        }
      }
    };
    if (instance.barConfig.islands) {
      collapseEdgeSpacers(instance.startWidgets);
      collapseEdgeSpacers(instance.centerWidgets);
      collapseEdgeSpacers(instance.endWidgets);
    }

    auto finalizeCapsules = [isVertical, capsuleCross, widgetHoverPadding = instance.barConfig.widgetCapsulePadding,
                             &renderer](std::vector<BarCapsuleRun>& runs) {
      for (auto& run : runs) {
        Node* shell = run.shell;
        Box* bg = run.bg;
        Node* content = run.content;
        if (shell == nullptr || bg == nullptr || content == nullptr) {
          continue;
        }
        if (run.container != nullptr) {
          run.container->layout(renderer);
        }

        bool hasVisibleContent = false;
        bool hasCapsuleContent = false;
        for (Widget* widget : run.widgets) {
          if (widget == nullptr || widget->root() == nullptr) {
            continue;
          }
          hasVisibleContent = hasVisibleContent || widget->root()->visible();
          hasCapsuleContent = hasCapsuleContent || widget->shouldShowBarCapsule();
        }
        const bool hasPaintedFill = resolveColorSpec(scaleAlpha(run.spec.fill, run.spec.opacity)).a > 0.0F;
        const bool hasPaintedBorder =
            run.spec.border.has_value() && run.spec.borderWidth > 0.0F && resolveColorSpec(*run.spec.border).a > 0.0F;
        run.hasPaintedCapsuleBackground = hasCapsuleContent && (hasPaintedFill || hasPaintedBorder);

        shell->setVisible(hasVisibleContent);
        const float scale = run.contentScale;
        const float iw = content->width();
        const float ih = content->height();
        auto memberMainPos = [isVertical](const Node* node) { return isVertical ? node->y() : node->x(); };
        auto memberMainExtent = [isVertical](const Node* node) { return isVertical ? node->height() : node->width(); };
        auto clearAccordionSuppression = [&run] {
          for (Widget* widget : run.widgets) {
            if (widget != nullptr) {
              widget->setBarPointerSuppressed(false);
            }
          }
        };
        if (!hasCapsuleContent) {
          shell->setSize(iw, ih);
          if (run.accordionClip != nullptr) {
            run.accordionClip->setPosition(0.0F, 0.0F);
            run.accordionClip->setSize(iw, ih);
          }
          content->setPosition(0.0F, 0.0F);
          bg->setVisible(false);
          bg->setPosition(0.0F, 0.0F);
          bg->setSize(iw, ih);
          placeCapsuleHoverBoxes(run, isVertical, iw, ih, 0.0F, 0.0F, std::min(iw, ih) * 0.5F, widgetHoverPadding);
          // No pill to hover: the group renders full-size, so nothing may stay input-suppressed.
          if (run.accordion) {
            clearAccordionSuppression();
          }
          continue;
        }
        const float pad = run.spec.padding * scale;
        const float padMain = pad;
        const float fullMain = isVertical ? ih : iw;
        // Collapsed accordion: the shell only spans its always-visible member; the rest are clipped
        // out and revealed as the hover progress lerps the main extent up to the full content size.
        const bool accordionStartDir = run.accordion && run.accordionDirection == BarAccordionDirection::Start;
        float revealMain = fullMain;
        if (run.accordion) {
          const Node* visibleMember = nullptr;
          auto laidOut = [](const Widget* widget) {
            const Node* node = widget != nullptr ? widget->outerNode() : nullptr;
            return node != nullptr && node->visible() && node->participatesInLayout() ? node : nullptr;
          };
          if (run.accordionVisibleIndex < run.widgets.size()) {
            visibleMember = laidOut(run.widgets[run.accordionVisibleIndex]);
          }
          if (visibleMember == nullptr) {
            // The configured first member is hidden: anchor on the laid-out member at the pill's fixed edge.
            for (Widget* widget : run.widgets) {
              if (const Node* node = laidOut(widget); node != nullptr) {
                visibleMember = node;
                if (!accordionStartDir) {
                  break;
                }
              }
            }
          }
          if (visibleMember != nullptr) {
            const float visMain = memberMainExtent(visibleMember);
            const float progress = std::clamp(run.accordionProgress, 0.0F, 1.0F);
            revealMain = visMain + (fullMain - visMain) * progress;
          }
          // Match Flex's logical-pixel positions so right-anchored siblings keep a stable rounding phase.
          revealMain = std::ceil(revealMain);
        }
        // Cross-size is the fixed capsuleCross, independent of per-widget content scale: scaling a widget
        // enlarges its glyph/text inside the fixed-height pill rather than resizing the capsule (so a
        // differently scaled member can't grow or split its capsule group). The main axis is content plus
        // per-widget padding, so an icon-only widget reads as a near-circular pill at the default padding
        // and widens as padding increases.
        const float shellMain = revealMain + 2.0F * padMain;
        const float shellCross = capsuleCross;
        const float shellW = isVertical ? shellCross : shellMain;
        const float shellH = isVertical ? shellMain : shellCross;
        // Start-direction accordions pin the content's end edge, so the always-visible last member
        // stays put while the hidden ones unfold off the pill's leading edge.
        const float contentMain = accordionStartDir ? shellMain - padMain - fullMain : padMain;
        const float contentX = isVertical ? (shellW - iw) * 0.5F : contentMain;
        const float contentY = isVertical ? contentMain : (shellH - ih) * 0.5F;
        shell->setSize(shellW, shellH);
        bg->setVisible(true);
        bg->setPosition(0.0F, 0.0F);
        bg->setSize(shellW, shellH);
        if (run.accordionClip != nullptr) {
          // Reveal window: [padMain, padMain + revealMain] in shell coordinates, for both directions.
          if (isVertical) {
            run.accordionClip->setPosition(0.0F, padMain);
            run.accordionClip->setSize(shellCross, revealMain);
          } else {
            run.accordionClip->setPosition(padMain, 0.0F);
            run.accordionClip->setSize(revealMain, shellCross);
          }
          content->setPosition(contentX - (isVertical ? 0.0F : padMain), contentY - (isVertical ? padMain : 0.0F));
        } else {
          content->setPosition(contentX, contentY);
        }
        const Widget* radiusSource = !run.widgets.empty() ? run.widgets.front() : nullptr;
        const float capsuleRadius = radiusSource != nullptr ? radiusSource->resolvedBarCapsuleRadius(shellW, shellH)
                                                            : std::max(0.0F, std::min(shellW, shellH) * 0.5F);
        bg->setRadius(capsuleRadius);
        if (run.container == nullptr) {
          placeCapsuleHoverBoxes(
              run, isVertical, shellW, shellH, contentX, contentY, capsuleRadius, widgetHoverPadding
          );
        }
        // Members outside the reveal window are clipped out; while collapsed (or collapsing) the
        // non-primary members are pointer-suppressed by reveal window so their hidden slots don't
        // capture clicks. While expanded, every valid layout member stays unsuppressed so hover
        // tracking and member interactions remain active across the reveal animation.
        if (run.accordion) {
          for (Widget* widget : run.widgets) {
            const Node* node = widget != nullptr ? widget->outerNode() : nullptr;
            if (widget == nullptr || node == nullptr || !node->visible() || !node->participatesInLayout()) {
              if (widget != nullptr) {
                widget->setBarPointerSuppressed(false);
              }
              continue;
            }
            if (run.accordionExpanded) {
              widget->setBarPointerSuppressed(false);
            } else {
              const float memberStart = contentMain + memberMainPos(node);
              const float memberEnd = memberStart + memberMainExtent(node);
              const bool memberFullyVisible = memberStart >= padMain - 0.5F && memberEnd <= padMain + revealMain + 0.5F;
              widget->setBarPointerSuppressed(!memberFullyVisible);
            }
          }
        }
      }
    };
    finalizeCapsules(instance.startCapsuleRuns);
    finalizeCapsules(instance.centerCapsuleRuns);
    finalizeCapsules(instance.endCapsuleRuns);

    // When bar touches screen edge, put the padding inside the sections, and extend the hit targets of
    // the first/last widgets to cover the area. So clicking on the screen edge still triggers the widget.
    const float paddingEnd = barMainPaddingEnd(instance.barConfig);
    const bool screenEdgeClick = instance.barConfig.marginEnds == 0 && !instance.barConfig.islands && padding > 0;
    const float paddingInsideSection = screenEdgeClick ? padding : 0.0F;
    const float paddingInsideSectionEnd = screenEdgeClick ? paddingEnd : 0.0F;
    const float contentMainStart = screenEdgeClick ? 0.0F : padding;
    const float contentMainEnd =
        std::max(contentMainStart, (isVertical ? barAreaH : barAreaW) - (screenEdgeClick ? 0.0F : paddingEnd));
    const float contentMainSpan = std::max(0.0F, contentMainEnd - contentMainStart);

    auto configureSlot = [&](Node* slot, float mainOffset, float mainSize) {
      slot->setClipChildren(true);
      if (isVertical) {
        slot->setPosition(0.0F, mainOffset);
        slot->setSize(slotCross, mainSize);
      } else {
        slot->setPosition(mainOffset, 0.0F);
        slot->setSize(mainSize, slotCross);
      }
    };

    auto configureSection = [&](Flex* section, FlexJustify justify) {
      section->setJustify(justify);
      section->layout(renderer);
    };

    if (screenEdgeClick) {
      if (isVertical) {
        instance.startSection->setPadding(paddingInsideSection, 0.0F, 0.0F, 0.0F);
        instance.endSection->setPadding(0.0F, 0.0F, paddingInsideSectionEnd, 0.0F);
      } else {
        instance.startSection->setPadding(0.0F, 0.0F, 0.0F, paddingInsideSection);
        instance.endSection->setPadding(0.0F, paddingInsideSectionEnd, 0.0F, 0.0F);
      }
    } else {
      instance.startSection->setPadding(0.0F);
      instance.endSection->setPadding(0.0F);
    }

    configureSection(instance.startSection, FlexJustify::Start);
    configureSection(instance.centerSection, FlexJustify::Center);
    configureSection(instance.endSection, FlexJustify::End);

    // Anchor mode: if a center widget is flagged as the anchor, pin its center to the
    // bar midline so surrounding siblings growing/shrinking cannot drift it sideways.
    const Node* anchorNode = nullptr;
    for (const auto& widget : instance.centerWidgets) {
      if (widget != nullptr && widget->isAnchor() && widget->layoutBoundsNode() != nullptr) {
        anchorNode = widget->layoutBoundsNode();
        break;
      }
    }

    const float barMidline = contentMainStart + contentMainSpan * 0.5F;
    const float centerNaturalMain = isVertical ? instance.centerSection->height() : instance.centerSection->width();

    float centerSlotStart;
    float centerSlotMain;
    float centerSectionOffset; // offset of section origin within its slot along main axis
    if (anchorNode != nullptr) {
      const float anchorOffsetInSection = isVertical ? anchorNode->y() : anchorNode->x();
      const float anchorSpan = isVertical ? anchorNode->height() : anchorNode->width();
      const float anchorCenterInSection = anchorOffsetInSection + anchorSpan * 0.5F;
      // Place the section so that the anchor's center sits at barMidline.
      float desiredSectionStart = barMidline - anchorCenterInSection;
      // Clamp so the section stays within the content area.
      const float maxStart = contentMainEnd - centerNaturalMain;
      desiredSectionStart = std::clamp(desiredSectionStart, contentMainStart, std::max(contentMainStart, maxStart));
      centerSlotStart = desiredSectionStart;
      centerSlotMain = std::min(centerNaturalMain, contentMainEnd - centerSlotStart);
      centerSectionOffset = 0.0F;
    } else {
      centerSlotMain = std::min(contentMainSpan, centerNaturalMain);
      centerSlotStart = contentMainStart + std::max(0.0F, (contentMainSpan - centerSlotMain) * 0.5F);
      centerSectionOffset = (centerSlotMain - centerNaturalMain) * 0.5F;
    }
    const float centerSlotEnd = centerSlotStart + centerSlotMain;
    float startSlotMain;
    float endSlotMain;
    if (!instance.centerWidgets.empty()) {
      startSlotMain = std::max(0.0F, centerSlotStart - contentMainStart);
      endSlotMain = std::max(0.0F, contentMainEnd - centerSlotEnd);
      configureSlot(instance.startSlot, contentMainStart, startSlotMain);
      configureSlot(instance.centerSlot, centerSlotStart, centerSlotMain);
      configureSlot(instance.endSlot, centerSlotEnd, endSlotMain);
    } else {
      // Allow start/end sections to take the full width if center is empty
      const float startNaturalMain = isVertical ? instance.startSection->height() : instance.startSection->width();
      const float endNaturalMain = isVertical ? instance.endSection->height() : instance.endSection->width();

      // Prioritize end section, because control center is likely to be there, and we don't
      // want it to be clipped by a super long start section, so the user loses access to settings.
      endSlotMain = std::min(endNaturalMain, contentMainSpan);
      startSlotMain = std::min(startNaturalMain, contentMainSpan - endSlotMain);
      configureSlot(instance.startSlot, contentMainStart, startSlotMain);
      configureSlot(instance.centerSlot, contentMainStart + startSlotMain, 0.0F);
      configureSlot(instance.endSlot, contentMainEnd - endSlotMain, endSlotMain);
    }

    if (isVertical) {
      instance.startSection->setPosition((slotCross - instance.startSection->width()) * 0.5F, 0.0F);
      instance.centerSection->setPosition((slotCross - instance.centerSection->width()) * 0.5F, centerSectionOffset);
      instance.endSection->setPosition(
          (slotCross - instance.endSection->width()) * 0.5F, endSlotMain - instance.endSection->height()
      );
    } else {
      instance.startSection->setPosition(0.0F, (slotCross - instance.startSection->height()) * 0.5F);
      instance.centerSection->setPosition(centerSectionOffset, (slotCross - instance.centerSection->height()) * 0.5F);
      instance.endSection->setPosition(
          endSlotMain - instance.endSection->width(), (slotCross - instance.endSection->height()) * 0.5F
      );
    }

    applyBarWidgetHitTargets(instance.startSection, instance.startSlot, isVertical);
    applyBarWidgetHitTargets(instance.startSection, instance.startSlot, isVertical);
    applyBarWidgetHitTargets(instance.centerSection, instance.centerSlot, isVertical);
    applyBarWidgetHitTargets(instance.endSection, instance.endSlot, isVertical);
    extendCapsuleHitTargets(instance.startCapsuleRuns, isVertical, instance.barConfig.widgetCapsulePadding);
    extendCapsuleHitTargets(instance.centerCapsuleRuns, isVertical, instance.barConfig.widgetCapsulePadding);
    extendCapsuleHitTargets(instance.endCapsuleRuns, isVertical, instance.barConfig.widgetCapsulePadding);

    // Ghost pills for capsule-less widgets: positioned on the bar-level underlay with the
    // metrics an enabled capsule would have (capsuleCross across the bar, capsule padding
    // along it). Runs after sections are positioned so absolute coordinates are final; the
    // widget's hit target is widened to match the pill.
    if (instance.hoverUnderlay != nullptr) {
      float underlayX = 0.0F;
      float underlayY = 0.0F;
      Node::absolutePosition(instance.hoverUnderlay, underlayX, underlayY);
      auto placeGhostPills = [&](std::vector<std::unique_ptr<Widget>>& widgets) {
        for (auto& widget : widgets) {
          Box* box = widget->barHoverBox();
          Node* root = widget->outerNode();
          if (box == nullptr || root == nullptr || widget->barCapsuleShell() != nullptr) {
            continue;
          }
          if (!root->visible() || !root->participatesInLayout()) {
            box->setSize(0.0F, 0.0F);
            continue;
          }
          float rootX = 0.0F;
          float rootY = 0.0F;
          Node::absolutePosition(root, rootX, rootY);
          rootX -= underlayX;
          rootY -= underlayY;
          const float mainExtent = isVertical ? root->height() : root->width();
          const float pad = mainExtent > 0.5F ? widget->barCapsuleSpec().padding * widget->contentScale() : 0.0F;
          const float hoverW = isVertical ? capsuleCross : root->width() + 2.0F * pad;
          const float hoverH = isVertical ? root->height() + 2.0F * pad : capsuleCross;
          box->setPosition(
              isVertical ? rootX + (root->width() - capsuleCross) * 0.5F : rootX - pad,
              isVertical ? rootY - pad : rootY + (root->height() - capsuleCross) * 0.5F
          );
          box->setSize(hoverW, hoverH);
          box->setRadius(widget->resolvedBarCapsuleRadius(hoverW, hoverH));
          if (auto* area = dynamic_cast<InputArea*>(root)) {
            auto outset = area->hitTestOutset();
            if (isVertical) {
              outset.top += pad;
              outset.bottom += pad;
            } else {
              outset.left += pad;
              outset.right += pad;
            }
            area->setHitTestOutset(outset);
          }
        }
      };
      placeGhostPills(instance.startWidgets);
      placeGhostPills(instance.centerWidgets);
      placeGhostPills(instance.endWidgets);

      auto placeGroupHoverPills = [&](std::vector<BarCapsuleRun>& runs) {
        for (auto& run : runs) {
          if (run.container == nullptr || run.shell == nullptr) {
            continue;
          }
          float shellX = 0.0F;
          float shellY = 0.0F;
          Node::absolutePosition(run.shell, shellX, shellY);
          shellX -= underlayX;
          shellY -= underlayY;
          const float shellMainStart = isVertical ? shellY : shellX;
          const Widget* radiusSource = !run.widgets.empty() ? run.widgets.front() : nullptr;
          for (Widget* widget : run.widgets) {
            Node* root = widget != nullptr ? widget->outerNode() : nullptr;
            Box* box = widget != nullptr ? widget->barHoverBox() : nullptr;
            if (box != nullptr
                && (root == nullptr || !root->visible() || !root->participatesInLayout() || !root->hitTestVisible())) {
              box->setSize(0.0F, 0.0F);
            }
          }

          const auto laidOut = capsuleMemberSlices(run, isVertical, instance.barConfig.widgetCapsulePadding);
          for (const auto& member : laidOut) {
            Widget* widget = run.widgets[member.widgetIndex];
            Box* box = widget->barHoverBox();
            if (box == nullptr) {
              continue;
            }

            const float rootX = shellX + member.x;
            const float rootY = shellY + member.y;
            const float mainStart = shellMainStart + member.sliceStart;
            const float mainEnd = shellMainStart + member.sliceEnd;
            const float mainExtent = std::max(0.0F, mainEnd - mainStart);
            const float hoverW = isVertical ? capsuleCross : mainExtent;
            const float hoverH = isVertical ? mainExtent : capsuleCross;
            box->setPosition(
                isVertical ? rootX + (member.width - capsuleCross) * 0.5F : mainStart,
                isVertical ? mainStart : rootY + (member.height - capsuleCross) * 0.5F
            );
            box->setSize(hoverW, hoverH);
            box->setRadius(
                radiusSource != nullptr ? radiusSource->resolvedBarCapsuleRadius(hoverW, hoverH)
                                        : widget->resolvedBarCapsuleRadius(hoverW, hoverH)
            );
          }
        }
      };
      placeGroupHoverPills(instance.startCapsuleRuns);
      placeGroupHoverPills(instance.centerCapsuleRuns);
      placeGroupHoverPills(instance.endCapsuleRuns);
    }
    if (screenEdgeClick) {
      auto extendHitTestOutsetToScreenEdge = [&](Node* node, bool start) {
        if (node == nullptr) {
          return;
        }
        auto hitTestOutset = node->hitTestOutset();
        if (start) {
          if (isVertical) {
            hitTestOutset.top += paddingInsideSection;
          } else {
            hitTestOutset.left += paddingInsideSection;
          }
        } else {
          if (isVertical) {
            hitTestOutset.bottom += paddingInsideSection;
          } else {
            hitTestOutset.right += paddingInsideSection;
          }
        }
        node->setHitTestOutset(hitTestOutset);
      };
      if (!instance.startWidgets.empty()) {
        auto* widget = instance.startWidgets.front().get();
        extendHitTestOutsetToScreenEdge(widget->outerNode(), true);
        extendHitTestOutsetToScreenEdge(widget->root(), true);
      }
      if (!instance.endWidgets.empty()) {
        auto* widget = instance.endWidgets.back().get();
        extendHitTestOutsetToScreenEdge(widget->outerNode(), false);
        extendHitTestOutsetToScreenEdge(widget->root(), false);
      }
    }
    layoutBarIslands(instance, isVertical);
  }


} // namespace

void Bar::buildScene(BarInstance& instance, std::uint32_t width, std::uint32_t height) {
  uiAssertNotRendering("Bar::buildScene");
  if (m_renderContext == nullptr || instance.surface == nullptr) {
    return;
  }
  Renderer& renderer = instance.surface->renderTarget().renderer();

  const auto w = static_cast<float>(width);
  const auto h = static_cast<float>(height);
  const float padding = barMainPadding(instance.barConfig);
  const auto widgetSpacing = static_cast<float>(instance.barConfig.widgetSpacing);
  const auto& shadowConfig = m_config->config().shell.shadow;
  const auto shadowOffset = shadowDirectionOffset(shadowConfig.direction);
  const float shadowSize = ui::surface_shadow::enabled(instance.barConfig.shadow, shadowConfig)
      ? static_cast<float>(ui::surface_shadow::kBlurRadius)
      : 0.0F;
  const auto shadowOffsetX = static_cast<float>(shadowOffset.x);
  const auto shadowOffsetY = static_cast<float>(shadowOffset.y);
  const bool isBottom = instance.barConfig.position == "bottom";
  const bool isRight = instance.barConfig.position == "right";
  const bool isVertical = (instance.barConfig.position == "left" || instance.barConfig.position == "right");
  const auto concave = barConcaveShape(instance.barConfig);

  const float innerSurfaceExtension = barInnerSurfaceExtension(instance.barConfig, shadowConfig, w, h);
  const auto barVisual = computeBarVisualGeometry(instance.barConfig, shadowConfig, w, h, innerSurfaceExtension);
  const float barAreaX = barVisual.x;
  const float barAreaY = barVisual.y;
  const float barAreaW = barVisual.width;
  const float barAreaH = barVisual.height;

  if (instance.sceneRoot == nullptr) {
    instance.sceneRoot = ui::node({});
    instance.sceneRoot->setAnimationManager(&instance.animations);
    instance.sceneRoot->setSize(w, h);

    auto slide = ui::node({});
    slide->setParticipatesInLayout(false);
    instance.slideRoot = instance.sceneRoot->addChild(std::move(slide));

    // Bar background
    instance.bg = static_cast<Box*>(instance.slideRoot->addChild(ui::box()));
    for (std::size_t i = 0; i < instance.islands.size(); ++i) {
      if (shadowSize > 0.0F) {
        auto shadow = ui::box();
        shadow->setHitTestVisible(false);
        shadow->setZIndex(-1);
        shadow->setVisible(false);
        instance.islandShadows[i] = static_cast<Box*>(instance.slideRoot->addChild(std::move(shadow)));
      }
      Box*& island = instance.islands[i];
      island = static_cast<Box*>(instance.slideRoot->addChild(ui::box()));
      island->setHitTestVisible(false);
      island->setVisible(instance.barConfig.islands);
    }

    // Shadow — bar shape copy rendered with large SDF softness to simulate a blurred drop shadow.
    if (shadowSize > 0.0F) {
      instance.shadow = static_cast<Box*>(instance.slideRoot->addChild(
          ui::box({
              .configure = [](Box& shadow) { shadow.setHitTestVisible(false); },
          })
      ));

      auto leftClip = ui::node({});
      leftClip->setClipChildren(true);
      leftClip->setZIndex(-1);
      instance.shadowLeftClip = instance.slideRoot->addChild(std::move(leftClip));
      instance.shadowLeft = static_cast<Box*>(instance.shadowLeftClip->addChild(ui::box()));

      auto rightClip = ui::node({});
      rightClip->setClipChildren(true);
      rightClip->setZIndex(-1);
      instance.shadowRightClip = instance.slideRoot->addChild(std::move(rightClip));
      instance.shadowRight = static_cast<Box*>(instance.shadowRightClip->addChild(ui::box()));
    }
    // Note: shadow is inserted before bar sections so it renders below them (z=-1 is set below).

    auto contentClip = ui::node({});
    contentClip->setClipChildren(true);
    instance.contentClip = instance.slideRoot->addChild(std::move(contentClip));

    auto hoverUnderlay = ui::node({});
    hoverUnderlay->setHitTestVisible(false);
    hoverUnderlay->setSize(static_cast<float>(w), static_cast<float>(h));
    instance.hoverUnderlay = instance.slideRoot->addChild(std::move(hoverUnderlay));

    auto makeSlot = [&instance] {
      auto slot = ui::node({});
      slot->setClipChildren(true);
      return instance.contentClip->addChild(std::move(slot));
    };
    instance.startSlot = makeSlot();
    instance.centerSlot = makeSlot();
    instance.endSlot = makeSlot();

    // Create section boxes
    auto makeSection = [widgetSpacing, isVertical] {
      auto section = ui::flex(
          isVertical ? FlexDirection::Vertical : FlexDirection::Horizontal,
          {
              .align = FlexAlign::Center,
              .gap = widgetSpacing,
          }
      );
      section->setMirrorInRtl(false);
      return section;
    };

    instance.startSection = static_cast<Flex*>(instance.startSlot->addChild(makeSection()));
    instance.centerSection = static_cast<Flex*>(instance.centerSlot->addChild(makeSection()));
    instance.endSection = static_cast<Flex*>(instance.endSlot->addChild(makeSection()));

    attachWidgetsToSections(instance);

    // Wire up InputDispatcher for this instance
    instance.inputDispatcher.setSceneRoot(instance.sceneRoot.get());
    instance.inputDispatcher.setCursorShapeCallback([this](std::uint32_t serial, std::uint32_t shape) {
      m_platform->setCursorShape(serial, shape);
    });
    instance.inputDispatcher.setHoverChangeCallback([this, inst = &instance](InputArea* /*old*/, InputArea* next) {
      if (next != nullptr) {
        next->setTooltipPlacement(tooltipPlacementAwayFromEdge(inst->barConfig.position));
      }
      TooltipManager::instance().onBarHoverChange(
          tooltipAreaForHover(*inst, next), inst->surface->layerSurface(), inst->output
      );
      updateWidgetHoverHighlight(*inst, next);
      updateAccordionExpansion(*inst, next);
    });

    if (instance.barConfig.smartAutoHide && m_platform != nullptr) {
      instance.smartAutoHidePinnedVisible = smartAutoHideWantsPinnedVisible(*m_platform, instance.output);
    }
    if (barConfigUsesSlideSurface(instance.barConfig)) {
      instance.slideRoot->setOpacity(1.0F);
      const bool startHidden =
          instance.barConfig.smartAutoHide ? !instance.smartAutoHidePinnedVisible : instance.barConfig.autoHide;
      instance.hideOpacity = startHidden ? 0.0F : 1.0F;
    } else {
      instance.slideRoot->setOpacity(1.0F);
      instance.hideOpacity = 1.0F;
    }

    instance.surface->setSceneRoot(instance.sceneRoot.get());
  }

  // Update root size on reconfigure
  instance.sceneRoot->setSize(w, h);
  if (instance.slideRoot != nullptr) {
    instance.slideRoot->setSize(w, h);
  }
  if (instance.hoverUnderlay != nullptr) {
    instance.hoverUnderlay->setSize(w, h);
  }

  // Background covers only the bar visual area (not the shadow extension).
  // Keep it exactly aligned with the shadow shape; the shadow shader now
  // draws only outside the rect, so any size mismatch is visible at corners.
  if (instance.bg != nullptr) {
    // In islands mode the island pills carry the material; the bar body stays unpainted.
    const bool paintBody = !instance.barConfig.islands;
    const RoundedRectStyle bgStyle{
        .fill = paintBody ? colorForRole(materialRole(), Style::glassAlpha(instance.barConfig.backgroundOpacity))
                          : rgba(0.0F, 0.0F, 0.0F, 0.0F),
        .border = resolveColorSpec(instance.barConfig.border),
        .fillMode = FillMode::Solid,
        .corners = concave.corners,
        .logicalInset = concave.logicalInset,
        .radius = concave.radii,
        .softness = 0.0F,
        .borderWidth = paintBody ? instance.barConfig.borderWidth : 0.0F,
    };
    instance.bg->setStyle(bgStyle);
    // (barAreaX/Y/W/H) is the body; the shader expands outward by logicalInset into
    // the visual rect, so the node must be sized to the visual rect.
    instance.bg->setPosition(barAreaX - concave.logicalInset.left, barAreaY - concave.logicalInset.top);
    instance.bg->setSize(
        barAreaW + concave.logicalInset.left + concave.logicalInset.right,
        barAreaH + concave.logicalInset.top + concave.logicalInset.bottom
    );
  }

  instance.paletteConn = paletteChanged().connect([inst = &instance] {
    applyBackgroundPalette(*inst);
    if (inst->surface != nullptr) {
      inst->surface->requestRedraw();
    }
  });
  applyBackgroundPalette(instance);
  if (instance.contentClip != nullptr) {
    instance.contentClip->setPosition(barAreaX, barAreaY);
    instance.contentClip->setSize(barAreaW, barAreaH);
  }

  applyBarShadowStyle(instance, shadowConfig, w, h);

  layoutBarSections(instance, renderer, barAreaW, barAreaH, padding, isVertical);
  notifyCenterIslandSpace(instance);

  float contentLeft = barAreaX;
  float contentTop = barAreaY;
  float contentRight = barAreaX + barAreaW;
  float contentBottom = barAreaY + barAreaH;
  if (instance.shadow != nullptr) {
    const float sx = barAreaX + shadowOffsetX;
    const float sy = barAreaY + shadowOffsetY;
    contentLeft = std::min(contentLeft, sx);
    contentTop = std::min(contentTop, sy);
    contentRight = std::max(contentRight, sx + barAreaW);
    contentBottom = std::max(contentBottom, sy + barAreaH);
  }
  // Concave spikes extend past the body on the inner edge; include them so the bar
  // slides fully off-screen when hidden.
  contentLeft -= concave.logicalInset.left;
  contentTop -= concave.logicalInset.top;
  contentRight += concave.logicalInset.right;
  contentBottom += concave.logicalInset.bottom;
  const auto hiddenDelta = computeAutoHideHiddenDelta(
      isVertical, isBottom, isRight, w, h, contentLeft, contentTop, contentRight, contentBottom
  );
  instance.slideHiddenDx = hiddenDelta.first;
  instance.slideHiddenDy = hiddenDelta.second;
  syncBarSlideLayerTransform(instance);

  // A rebuilt scene may sit on a new surface, so always send the region once.
  instance.clickThroughInputRegion.reset();
  syncBarAutoHideInputRegion(instance);
  syncBarSurfaceChrome(instance);  syncSheetTracker(instance);
}

void Bar::updateWidgets(BarInstance& instance) {
  if (m_renderContext == nullptr || instance.surface == nullptr) {
    return;
  }
  Renderer& renderer = instance.surface->renderTarget().renderer();

  const auto w = static_cast<float>(instance.surface->width());
  const auto h = static_cast<float>(instance.surface->height());
  const float padding = barMainPadding(instance.barConfig);
  const bool isVertical = (instance.barConfig.position == "left" || instance.barConfig.position == "right");
  const auto& shadowConfig = m_config->config().shell.shadow;
  const float innerSurfaceExtension = barInnerSurfaceExtension(instance.barConfig, shadowConfig, w, h);
  const auto barVisual = computeBarVisualGeometry(instance.barConfig, shadowConfig, w, h, innerSurfaceExtension);
  const float barAreaW = barVisual.width;
  const float barAreaH = barVisual.height;

  auto updateSection = [&](std::vector<std::unique_ptr<Widget>>& widgets) {
    for (auto& widget : widgets) {
      if (widget->root() == nullptr) {
        continue;
      }
      widget->update(renderer);
      widget->layout(renderer, barAreaW, barAreaH);
    }
  };

  updateSection(instance.startWidgets);
  updateSection(instance.centerWidgets);
  updateSection(instance.endWidgets);
  layoutBarSections(instance, renderer, barAreaW, barAreaH, padding, isVertical);
  notifyCenterIslandSpace(instance);
  if (instance.barConfig.deadZone.clickThrough) {
    syncBarAutoHideInputRegion(instance);
  }  syncSheetTracker(instance);
}

void Bar::prepareFrame(BarInstance& instance, bool needsUpdate, bool needsLayout) {
  if (m_renderContext == nullptr || instance.surface == nullptr) {
    return;
  }

  m_renderContext->makeCurrent(instance.surface->renderTarget());
  Renderer& renderer = instance.surface->renderTarget().renderer();

  if (needsUpdate) {
    UiPhaseScope updatePhase(UiPhase::Update);
    updateWidgets(instance);
    return;
  }

  if (!needsLayout) {
    return;
  }

  const auto w = static_cast<float>(instance.surface->width());
  const auto h = static_cast<float>(instance.surface->height());
  const float padding = barMainPadding(instance.barConfig);
  const bool isVertical = (instance.barConfig.position == "left" || instance.barConfig.position == "right");
  const auto& shadowConfig = m_config->config().shell.shadow;
  const float innerSurfaceExtension = barInnerSurfaceExtension(instance.barConfig, shadowConfig, w, h);
  const auto barVisual = computeBarVisualGeometry(instance.barConfig, shadowConfig, w, h, innerSurfaceExtension);
  const float barAreaW = barVisual.width;
  const float barAreaH = barVisual.height;

  {
    UiPhaseScope layoutPhase(UiPhase::Layout);
    for (auto& widget : instance.startWidgets) {
      widget->layout(renderer, barAreaW, barAreaH);
    }
    for (auto& widget : instance.centerWidgets) {
      widget->layout(renderer, barAreaW, barAreaH);
    }
    for (auto& widget : instance.endWidgets) {
      widget->layout(renderer, barAreaW, barAreaH);
    }
    layoutBarSections(instance, renderer, barAreaW, barAreaH, padding, isVertical);
    notifyCenterIslandSpace(instance);
  }
  // Click-through input follows the widgets, which move whenever a widget resizes.
  if (instance.barConfig.deadZone.clickThrough) {
    syncBarAutoHideInputRegion(instance);
  }
  syncSheetTracker(instance);
}

