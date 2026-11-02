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

  constexpr Logger kLog("bar");

} // namespace

namespace bar_detail {

  bool pointInsideNode(const Node* node, float sceneX, float sceneY) {
    if (node == nullptr) {
      return false;
    }
    float localX = 0.0F;
    float localY = 0.0F;
    if (!Node::mapFromScene(node, sceneX, sceneY, localX, localY)) {
      return false;
    }
    return localX >= 0.0F && localX < node->width() && localY >= 0.0F && localY < node->height();
  }


} // namespace

namespace {

  HitTestOutset crossAxisOutsetToSlot(const Node* node, const Node* slot, bool isVertical) {
    if (node == nullptr || slot == nullptr) {
      return {};
    }

    float nodeX = 0.0F;
    float nodeY = 0.0F;
    float slotX = 0.0F;
    float slotY = 0.0F;
    Node::absolutePosition(node, nodeX, nodeY);
    Node::absolutePosition(slot, slotX, slotY);

    if (isVertical) {
      return {
          .left = std::max(0.0F, nodeX - slotX),
          .top = 0.0F,
          .right = std::max(0.0F, (slotX + slot->width()) - (nodeX + node->width())),
          .bottom = 0.0F,
      };
    }

    return {
        .left = 0.0F,
        .top = std::max(0.0F, nodeY - slotY),
        .right = 0.0F,
        .bottom = std::max(0.0F, (slotY + slot->height()) - (nodeY + node->height())),
    };
  }


} // namespace

namespace bar_detail {

  void applyBarWidgetHitTargets(Node* node, const Node* slot, bool isVertical) {
    if (node == nullptr || slot == nullptr) {
      return;
    }

    if (dynamic_cast<InputArea*>(node) != nullptr || node->clipChildren()) {
      node->setHitTestOutset(crossAxisOutsetToSlot(node, slot, isVertical));
    }

    for (const auto& child : node->children()) {
      applyBarWidgetHitTargets(child.get(), slot, isVertical);
    }
  }

  Widget* widgetAtPoint(const std::vector<std::unique_ptr<Widget>>& widgets, float sceneX, float sceneY) {
    for (const auto& widgetPtr : std::views::reverse(widgets)) {
      auto* widget = widgetPtr.get();
      if (widget == nullptr
          || widget->isBarClickThrough()
          || widget->outerNode() == nullptr
          || !widget->outerNode()->visible()) {
        continue;
      }
      // Bounds only, never Node::hitTest: hit outsets deliberately extend a widget's clickable
      // band past its ink (bar.cpp applies the hover pill padding that way), and treating that
      // band as "on a widget" would carve unreachable holes out of the dead zone.
      if (pointInsideNode(widget->outerNode(), sceneX, sceneY)) {
        return widget;
      }
    }
    for (const auto& widgetPtr : std::views::reverse(widgets)) {
      auto* widget = widgetPtr.get();
      if (widget == nullptr || widget->isBarClickThrough()) {
        continue;
      }
      auto* root = widget != nullptr ? widget->outerNode() : nullptr;
      auto* bounds = widget != nullptr ? widget->layoutBoundsNode() : nullptr;
      if (root == nullptr || bounds == nullptr || bounds == root || root->parent() != bounds || !bounds->visible()) {
        continue;
      }
      if (pointInsideNode(bounds, sceneX, sceneY)) {
        return widget;
      }
    }
    return nullptr;
  }

  Widget* widgetAtPoint(const BarInstance& instance, float sceneX, float sceneY) {
    if (auto* widget = widgetAtPoint(instance.endWidgets, sceneX, sceneY); widget != nullptr) {
      return widget;
    }
    if (auto* widget = widgetAtPoint(instance.centerWidgets, sceneX, sceneY); widget != nullptr) {
      return widget;
    }
    return widgetAtPoint(instance.startWidgets, sceneX, sceneY);
  }


} // namespace

namespace {

  void tickWidgets(std::vector<std::unique_ptr<Widget>>& widgets, float deltaMs) {
    for (auto& widget : widgets) {
      if (widget != nullptr && widget->needsFrameTick()) {
        widget->onFrameTick(deltaMs);
      }
    }
  }

  bool widgetsNeedFrameTick(const std::vector<std::unique_ptr<Widget>>& widgets) {
    return std::ranges::any_of(widgets, [](const auto& widget) {
      return widget != nullptr && widget->needsFrameTick();
    });
  }

  // Walks up from the hovered area to the bar widget that owns it, if any.
  Widget* widgetFromHoveredArea(BarInstance& instance, InputArea* hoveredArea) {
    for (const Node* node = hoveredArea; node != nullptr; node = node->parent()) {
      if (auto it = instance.widgetByRoot.find(node); it != instance.widgetByRoot.end()) {
        return it->second;
      }
    }
    return nullptr;
  }

  // An accordion run wants to be expanded while one of its widgets is hovered or, once it has started
  // unfolding, while the pointer stays anywhere over its capsule (gaps and padding included).
  bool accordionRunWantsExpansion(const BarInstance& instance, const BarCapsuleRun& run, const Widget* target) {
    if (target != nullptr && std::ranges::contains(run.widgets, target)) {
      return true;
    }
    return (run.accordionExpanded || run.accordionProgress > 0.0F)
        && instance.pointerInside
        && pointInsideNode(run.shell, instance.lastPointerSx, instance.lastPointerSy);
  }


} // namespace

namespace bar_detail {

  // The area the tooltip manager may show a tooltip for: none when the owning widget disables tooltips.
  InputArea* tooltipAreaForHover(BarInstance& instance, InputArea* hoveredArea) {
    const Widget* owner = widgetFromHoveredArea(instance, hoveredArea);
    return owner != nullptr && !owner->showsTooltip() ? nullptr : hoveredArea;
  }


} // namespace

namespace {

  bool widgetsWantSecondTicks(const std::vector<std::unique_ptr<Widget>>& widgets) {
    return std::ranges::any_of(widgets, [](const auto& widget) {
      return widget != nullptr && widget->wantsSecondTicks();
    });
  }


} // namespace

void Bar::populateWidgets(BarInstance& instance) {
  instance.deadZoneBindings.resolve(
      noctalia::bar::WidgetActionBindings::Inputs{
          .widgetDefaults = noctalia::bar::deadZoneGestureDefaults(),
          .widgetActions = &instance.barConfig.deadZone.actions,
          .widgetContext = "bar." + instance.barConfig.name + ".dead_zone",
          .widgetName = instance.barConfig.name,
          .widgetType = "dead zone",
      }
  );
  {
    std::string summary;
    for (const auto gesture : noctalia::bar::allGestures()) {
      const auto* action = instance.deadZoneBindings.find(gesture);
      if (action == nullptr) {
        continue;
      }
      if (!summary.empty()) {
        summary += ", ";
      }
      summary += std::format("{}={}", gestureConfigKey(gesture), action->commandLine());
    }
    // Runs once per bar per reload, mirroring the per-widget line: the first thing to check when a
    // dead zone binding does not fire.
    kLog.debug("bar.{}.dead_zone: {}", instance.barConfig.name, summary.empty() ? "no bindings" : summary);
  }

  const auto& widgetConfigs = m_config->config().widgets;
  const auto labelFontWeight = static_cast<FontWeight>(instance.barConfig.fontWeight);
  const std::string barFontFamily = (instance.barConfig.fontFamily && !instance.barConfig.fontFamily->empty())
      ? *instance.barConfig.fontFamily
      : m_config->config().shell.fontFamily;
  // Creates one widget for `name`. When `groupSpec` is set the widget is a member of a capsule group and
  // takes the group's capsule style + foreground; otherwise it resolves its own per-widget/bar capsule.
  auto createWidget = [&](const std::string& name, const WidgetBarCapsuleSpec* groupSpec,
                          const std::optional<ColorSpec>* groupForeground, std::vector<std::unique_ptr<Widget>>& dest) {
    const WidgetConfig* wcPtr = nullptr;
    const std::string baseName = islandBaseWidgetName(name);
    if (auto it = widgetConfigs.find(baseName); it != widgetConfigs.end()) {
      wcPtr = &it->second;
    }
    const std::string_view widgetType =
        wcPtr != nullptr && !wcPtr->type.empty() ? std::string_view(wcPtr->type) : std::string_view(baseName);
    const CommonWidgetOptions options =
        resolveCommonWidgetOptions(instance.barConfig, wcPtr, widgetType, instance.barConfig.scale);
    if (!options.enabled) {
      return;
    }
    auto widget = m_widgetFactory->create(
        name, instance.output, options.contentScale, instance.barConfig.position, instance.barConfig.name,
        static_cast<float>(instance.barConfig.widgetSpacing), options.enableScroll
    );
    if (widget == nullptr) {
      return;
    }
    widget->setConfigName(name);
    const FontWeight islandWeight = name == "island_app" ? FontWeight::Bold : FontWeight::SemiBold;
    widget->applyCommonOptions(
        options, name.starts_with("island_") && name != "island_workspaces" ? islandWeight : labelFontWeight,
        barFontFamily, std::format("widget.{}", name)
    );
    widget->setActionContext(
        IpcInvocationContext{
            .widgetName = name,
            .widgetType = wcPtr != nullptr ? wcPtr->type : baseName,
            .barName = instance.barConfig.name,
            .output = instance.output,
        }
    );
    widget->resolveGestureBindings(
        wcPtr != nullptr ? wcPtr->type : baseName, wcPtr, &instance.barConfig.actions,
        std::format("bar.{}", instance.barConfig.name), &m_actionDispatcher
    );
    widget->setBarCapsuleSpec(groupSpec != nullptr ? *groupSpec : options.capsule);
    if (options.color.has_value()) {
      widget->setWidgetForeground(options.color);
    } else if (groupForeground != nullptr && groupForeground->has_value()) {
      widget->setWidgetForeground(*groupForeground);
    } else if (instance.barConfig.widgetColor.has_value()) {
      widget->setWidgetForeground(instance.barConfig.widgetColor);
    }
    if (options.iconColor.has_value()) {
      widget->setWidgetIconColor(options.iconColor);
    } else if (groupForeground != nullptr && groupForeground->has_value()) {
      widget->setWidgetIconColor(*groupForeground);
    } else if (instance.barConfig.widgetIconColor.has_value()) {
      widget->setWidgetIconColor(instance.barConfig.widgetIconColor);
    }
    dest.push_back(std::move(widget));
  };

  // Expands a lane's entries: group tokens become contiguous member widgets sharing the group's capsule.
  auto createWidgets = [&](const std::vector<std::string>& names, std::vector<std::unique_ptr<Widget>>& dest) {
    for (const auto& name : names) {
      if (isCapsuleGroupToken(name)) {
        const BarCapsuleGroupStyle* group = findBarCapsuleGroupStyle(instance.barConfig, capsuleGroupTokenId(name));
        if (group == nullptr) {
          kLog.warn("bar.{}: lane entry \"{}\" has no matching capsule_group", instance.barConfig.name, name);
          continue;
        }
        if (!group->enabled) {
          continue;
        }
        const WidgetBarCapsuleSpec groupSpec = capsuleSpecFromGroup(instance.barConfig, *group);
        for (const auto& member : group->members) {
          createWidget(member, &groupSpec, &group->foreground, dest);
        }
        continue;
      }
      createWidget(name, nullptr, nullptr, dest);
    }
  };

  createWidgets(instance.barConfig.startWidgets, instance.startWidgets);
  createWidgets(instance.barConfig.centerWidgets, instance.centerWidgets);
  createWidgets(instance.barConfig.endWidgets, instance.endWidgets);

#ifndef NDEBUG
  // Prepend a red "debug" pill to the end section if running a debug build
  auto debugWidget = m_widgetFactory->create(
      "debug_indicator", instance.output, instance.barConfig.scale, instance.barConfig.position,
      instance.barConfig.name, static_cast<float>(instance.barConfig.widgetSpacing)
  );
  if (debugWidget != nullptr) {
    debugWidget->setConfigName("debug_indicator");
    debugWidget->setFontScale(instance.barConfig.fontScale);
    debugWidget->setLabelFontWeight(labelFontWeight);
    debugWidget->setLabelFontFamily(barFontFamily);
    debugWidget->create();
    instance.endWidgets.insert(instance.endWidgets.begin(), std::move(debugWidget));
  }
#endif
}

void Bar::attachWidgetsToSections(BarInstance& instance) {
  const bool isVertical = instance.barConfig.position == "left" || instance.barConfig.position == "right";
  const auto widgetSpacing = static_cast<float>(instance.barConfig.widgetSpacing);
  const bool hoverHighlight = instance.barConfig.hoverHighlight;

  instance.widgetByRoot.clear();
  instance.hoverHighlightWidget = nullptr;
  if (instance.hoverUnderlay != nullptr) {
    while (!instance.hoverUnderlay->children().empty()) {
      instance.hoverUnderlay->removeChild(instance.hoverUnderlay->children().back().get());
    }
  }

  // Hover overlay: sits above the capsule fill (same zIndex, later sibling) and below the
  // content; fill/visibility are driven by the hover animation.
  auto addHoverBox = [hoverHighlight](Widget& widget, Node& shell) -> Box* {
    if (!hoverHighlight || !widget.wantsBarHoverHighlight()) {
      return nullptr;
    }
    Box* boxPtr = nullptr;
    shell.addChild(
        ui::box({
            .out = &boxPtr,
            .fill = scaleAlpha(widget.widgetForegroundOr(colorSpecFromRole(ColorRole::Label1)), 0.0F),
            .visible = false,
            .configure = [](Box& box) { box.setZIndex(-1); },
        })
    );
    widget.setBarHoverBox(boxPtr);
    return boxPtr;
  };

  auto attach = [&](std::vector<std::unique_ptr<Widget>>& widgets, std::vector<BarCapsuleRun>& capsuleRuns,
                    Flex* section) {
    if (section == nullptr) {
      return;
    }

    for (auto& widget : widgets) {
      widget->setAnimationManager(&instance.animations);
      widget->setUpdateCallback([surface = instance.surface.get()] {
        if (surface != nullptr) {
          surface->requestUpdate();
        }
      });
      widget->setRedrawCallback([surface = instance.surface.get()] {
        if (surface != nullptr) {
          surface->requestRedraw();
        }
      });
      widget->setFrameTickRequestCallback([surface = instance.surface.get()] {
        if (surface != nullptr) {
          surface->requestFrameTick();
        }
      });
      if (auto* plugin = dynamic_cast<PluginWidget*>(widget.get()); plugin != nullptr) {
        plugin->setUpdateDeferralCallback([] {
          auto* panel = PanelManager::current();
          return panel != nullptr && panel->isPanelTransitionActive();
        });
      }
      widget->setPanelToggleCallback([this, inst = &instance](
                                         std::string_view panelId, std::string_view context,
                                         std::optional<float> anchorSurfaceX, std::optional<float> anchorSurfaceY,
                                         Widget::PanelActivation activation
                                     ) {
        float anchorX = inst->lastPointerSx;
        float anchorY = inst->lastPointerSy;
        if (anchorSurfaceX.has_value()) {
          anchorX = *anchorSurfaceX;
        }
        if (anchorSurfaceY.has_value()) {
          anchorY = *anchorSurfaceY;
        }
        const auto island = islandRectAt(*inst, anchorX, anchorY);
        if (island) {
          anchorX = (*island)[0] + (*island)[2] * 0.5F;
          anchorY = (*island)[1] + (*island)[3] * 0.5F;
        }
        if (m_platform != nullptr && inst->output != nullptr) {
          if (const auto* out = m_platform->findOutputByWl(inst->output); out != nullptr && out->hasUsableGeometry()) {
            const auto [surfaceX, surfaceY] = surfaceOriginForOutputLocal(*inst, *out);
            anchorX += surfaceX;
            anchorY += surfaceY;
          }
        }
        PanelOpenRequest request{
            .output = inst->output,
            .anchorX = anchorX,
            .anchorY = anchorY,
            .hasExplicitAnchor = anchorSurfaceX.has_value() || anchorSurfaceY.has_value(),
            .hasAnchorPosition = true,
            .context = context,
            .sourceBarName = inst->barConfig.name,
            .sourceSurface = island && inst->surface != nullptr ? inst->surface->wlSurface() : nullptr,
            .sourceRect = island.value_or(std::array<float, 4>{}),
        };
        if (activation == Widget::PanelActivation::Open) {
          PanelManager::instance().openPanel(std::string(panelId), request);
        } else {
          PanelManager::instance().togglePanel(std::string(panelId), request);
        }
        if (PanelManager::instance().isOpenPanel(panelId)) {
          suppressTooltipForOpenPanel(panelId);
        }
      });
      if (auto* tray = dynamic_cast<TrayWidget*>(widget.get())) {
        tray->setHoverOverlayParent(instance.hoverUnderlay);
      }

      widget->create();
      if (widget->outerNode() != nullptr) {
        instance.widgetByRoot[widget->outerNode()] = widget.get();
      }
    }

    capsuleRuns.clear();

    auto addPlainWidget = [&](Widget& widget) {
      widget.setBarCapsuleScene(nullptr, nullptr);
      // No capsule: the hover pill lives on the bar-level underlay (unclipped, no layout
      // footprint) and is positioned after sections are laid out.
      if (hoverHighlight
          && instance.hoverUnderlay != nullptr
          && !widget.isBarClickThrough()
          && !widget.noGapAroundMe()) {
        addHoverBox(widget, *instance.hoverUnderlay);
      }
      auto* added = section->addChild(widget.releaseRoot());
      if (widget.noGapAroundMe()) {
        section->setChildGapExcluded(added, true);
      }
    };

    auto addSingleCapsule = [&](Widget& widget) {
      const auto& cap = widget.barCapsuleSpec();
      auto shell = ui::node({});
      Node* shellPtr = shell.get();
      shellPtr->setClipChildren(true);
      const float scale = widget.contentScale();
      Box* bgPtr = nullptr;
      auto capsuleBg = ui::box({
          .out = &bgPtr,
          .fill = scaleAlpha(cap.fill, cap.opacity),
          .configure = [&cap, scale](Box& bg) {
            if (cap.border.has_value()) {
              bg.setBorder(*cap.border, cap.borderWidth * scale);
            } else {
              bg.clearBorder();
            }
            bg.setZIndex(-1);
          },
      });
      shellPtr->addChild(std::move(capsuleBg));
      Box* hoverPtr = addHoverBox(widget, *shellPtr);
      shellPtr->addChild(widget.releaseRoot());
      widget.setBarCapsuleScene(shellPtr, bgPtr);
      if (auto* area = dynamic_cast<InputArea*>(widget.root())) {
        area->setTooltipAnchorNode(shellPtr);
      }
      capsuleRuns.push_back(
          BarCapsuleRun{
              .shell = shellPtr,
              .bg = bgPtr,
              .container = nullptr,
              .content = widget.outerNode(),
              .spec = cap,
              .contentScale = widget.contentScale(),
              .widgets = {&widget},
              .hoverBoxes = hoverPtr != nullptr ? std::vector<Box*>{hoverPtr} : std::vector<Box*>{},
          }
      );
      auto* added = section->addChild(std::move(shell));
      if (widget.noGapAroundMe()) {
        section->setChildGapExcluded(added, true);
      }
    };

    // Members of the same group share one resolved style by construction (see resolveWidgetBarCapsuleSpec),
    // so adjacency + matching group ID is sufficient to merge. Per-widget scale does not split the group:
    // the run is sized from its largest member's scale below so a differently scaled member still fits.
    auto canJoinCapsuleGroup = [](const Widget& first, const Widget& next) {
      const auto& firstSpec = first.barCapsuleSpec();
      const auto& nextSpec = next.barCapsuleSpec();
      return firstSpec.enabled
          && nextSpec.enabled
          && !first.isAnchor()
          && !next.isAnchor()
          && !firstSpec.group.empty()
          && firstSpec.group == nextSpec.group;
    };

    std::size_t index = 0;
    while (index < widgets.size()) {
      auto& widget = widgets[index];
      if (widget->root() == nullptr) {
        ++index;
        continue;
      }

      const auto& cap = widget->barCapsuleSpec();
      if (!cap.enabled) {
        addPlainWidget(*widget);
        ++index;
        continue;
      }

      if (widget->isAnchor() || cap.group.empty()) {
        addSingleCapsule(*widget);
        ++index;
        continue;
      }

      std::size_t runEnd = index + 1;
      while (runEnd < widgets.size()
             && widgets[runEnd] != nullptr
             && widgets[runEnd]->root() != nullptr
             && canJoinCapsuleGroup(*widget, *widgets[runEnd])) {
        ++runEnd;
      }

      if (runEnd - index < 2) {
        addSingleCapsule(*widget);
        ++index;
        continue;
      }

      auto shell = ui::node({});
      Node* shellPtr = shell.get();
      shellPtr->setClipChildren(true);
      const float scale = widget->contentScale();
      Box* bgPtr = nullptr;
      auto capsuleBg = ui::box({
          .out = &bgPtr,
          .fill = scaleAlpha(cap.fill, cap.opacity),
          .configure = [&cap, scale](Box& bg) {
            if (cap.border.has_value()) {
              bg.setBorder(*cap.border, cap.borderWidth * scale);
            } else {
              bg.clearBorder();
            }
            bg.setZIndex(-1);
          },
      });
      shellPtr->addChild(std::move(capsuleBg));

      // Visual order: with accordion + Start direction the always-visible first member sits last,
      // so the hidden members unfold before it along the lane's main axis. Flex children,
      // run.widgets and run.hoverBoxes must all follow this same order.
      const bool accordion = cap.accordion && (runEnd - index >= 2);
      const bool accordionStart = accordion && cap.accordionDirection == BarAccordionDirection::Start;
      std::vector<std::size_t> memberOrder;
      memberOrder.reserve(runEnd - index);
      if (accordionStart) {
        for (std::size_t memberIndex = index + 1; memberIndex < runEnd; ++memberIndex) {
          memberOrder.push_back(memberIndex);
        }
        memberOrder.push_back(index);
      } else {
        for (std::size_t memberIndex = index; memberIndex < runEnd; ++memberIndex) {
          memberOrder.push_back(memberIndex);
        }
      }

      std::vector<Box*> hoverBoxes;
      if (hoverHighlight) {
        hoverBoxes.reserve(memberOrder.size());
        for (const std::size_t memberIndex : memberOrder) {
          hoverBoxes.push_back(
              instance.hoverUnderlay != nullptr ? addHoverBox(*widgets[memberIndex], *instance.hoverUnderlay) : nullptr
          );
        }
      }

      auto inner = ui::flex(
          isVertical ? FlexDirection::Vertical : FlexDirection::Horizontal,
          {
              .align = FlexAlign::Center,
              .gap = cap.widgetSpacing.value_or(widgetSpacing),
          }
      );
      Flex* innerPtr = inner.get();
      // Accordion members are clipped to the reveal window, not the padded shell: the capsule
      // padding is wider than the member gap at larger padding values, which would otherwise leave
      // the next member's leading edge showing inside the collapsed pill.
      Node* clipPtr = nullptr;
      if (accordion) {
        auto clip = ui::node({});
        clipPtr = clip.get();
        clipPtr->setClipChildren(true);
        clipPtr->addChild(std::move(inner));
        shellPtr->addChild(std::move(clip));
      } else {
        shellPtr->addChild(std::move(inner));
      }

      BarCapsuleRun run;
      run.shell = shellPtr;
      run.bg = bgPtr;
      run.container = innerPtr;
      run.content = innerPtr;
      run.spec = cap;
      run.contentScale = widget->contentScale();
      run.hoverBoxes = std::move(hoverBoxes);
      run.accordion = accordion;
      run.accordionDelayMs = cap.accordionDelayMs.value_or(0.0F);
      run.accordionClip = clipPtr;
      run.accordionDirection = cap.accordionDirection;
      run.accordionVisibleIndex = accordionStart ? memberOrder.size() - 1 : 0;

      for (const std::size_t memberIndex : memberOrder) {
        auto& member = widgets[memberIndex];
        member->setBarCapsuleScene(shellPtr, bgPtr);
        run.widgets.push_back(member.get());
        auto* added = innerPtr->addChild(member->releaseRoot());
        if (auto* area = dynamic_cast<InputArea*>(member->root())) {
          area->setTooltipAnchorNode(shellPtr);
        }
        if (member->noGapAroundMe()) {
          innerPtr->setChildGapExcluded(added, true);
        }
      }

      capsuleRuns.push_back(std::move(run));
      section->addChild(std::move(shell));
      index = runEnd;
    }
  };

  attach(instance.startWidgets, instance.startCapsuleRuns, instance.startSection);
  attach(instance.centerWidgets, instance.centerCapsuleRuns, instance.centerSection);
  attach(instance.endWidgets, instance.endCapsuleRuns, instance.endSection);
}

void Bar::updateWidgetHoverHighlight(BarInstance& instance, InputArea* hoveredArea) {
  Widget* target = widgetFromHoveredArea(instance, hoveredArea);
  if (target != nullptr && (target->barHoverBox() == nullptr || !target->wantsBarHoverTint())) {
    target = nullptr;
  }
  if (target == instance.hoverHighlightWidget) {
    return;
  }
  if (instance.hoverHighlightWidget != nullptr) {
    animateWidgetHoverHighlight(instance, *instance.hoverHighlightWidget, false);
  }
  instance.hoverHighlightWidget = target;
  if (target != nullptr) {
    animateWidgetHoverHighlight(instance, *target, true);
  }
}

void Bar::animateWidgetHoverHighlight(BarInstance& instance, Widget& widget, bool hovered) {
  Box* box = widget.barHoverBox();
  if (box == nullptr) {
    return;
  }
  const ColorSpec fill = widget.widgetForegroundOr(colorSpecFromRole(ColorRole::Label1));
  instance.animations.cancelForOwner(box);
  instance.animations.animateSpring(
      widget.barHoverProgress(), hovered ? 1.0F : 0.0F, Style::spring(hovered ? Tokens::springFadeOut : Tokens::springFade),
      [&widget, box, fill](float progress) {
        widget.setBarHoverProgress(progress);
        box->setVisible(progress > 0.001F);
        box->setFill(scaleAlpha(fill, kWidgetHoverFillAlpha * progress));
      },
      {}, box
  );
  if (instance.surface != nullptr) {
    instance.surface->requestRedraw();
  }
}

void Bar::beginAccordionAnimation(BarInstance& instance, BarCapsuleRun& run, bool expanded) {
  run.accordionExpanded = expanded;
  instance.animations.cancelForOwner(run.shell);
  instance.animations.animateSpring(
      run.accordionProgress, expanded ? 1.0F : 0.0F, Style::spring(Tokens::springMove),
      [&run, surface = instance.surface.get()](float progress) {
        run.accordionProgress = progress;
        if (surface != nullptr) {
          surface->requestLayout();
        }
      },
      {}, run.shell
  );
}

void Bar::updateAccordionExpansion(BarInstance& instance, InputArea* hoveredArea) {
  Widget* target = widgetFromHoveredArea(instance, hoveredArea);

  bool changed = false;
  auto updateRuns = [&](std::vector<BarCapsuleRun>& runs) {
    for (auto& run : runs) {
      if (!run.accordion || run.shell == nullptr) {
        run.accordionDelayTimer.stop();
        continue;
      }
      const bool want = accordionRunWantsExpansion(instance, run, target);
      if (want == run.accordionExpanded) {
        run.accordionDelayTimer.stop();
        continue;
      }
      if (!want) {
        // Collapse stays immediate; a pending expand is void once the pointer leaves.
        run.accordionDelayTimer.stop();
        beginAccordionAnimation(instance, run, false);
        changed = true;
        continue;
      }
      // Expand honors accordion_delay_ms: arm a one-shot and re-verify hover when it fires, so a
      // pointer that already moved on never unfolds the group.
      if (run.accordionDelayMs <= 0.0F) {
        beginAccordionAnimation(instance, run, true);
        changed = true;
        continue;
      }
      if (run.accordionDelayTimer.active()) {
        continue;
      }
      run.accordionDelayTimer.start(
          std::chrono::milliseconds(static_cast<int>(run.accordionDelayMs)), [this, &instance, &run] {
            const Widget* nowTarget = widgetFromHoveredArea(instance, instance.inputDispatcher.hoveredArea());
            if (run.accordionExpanded || !accordionRunWantsExpansion(instance, run, nowTarget)) {
              return;
            }
            beginAccordionAnimation(instance, run, true);
            if (instance.surface != nullptr) {
              instance.surface->requestLayout();
            }
          }
      );
    }
  };
  updateRuns(instance.startCapsuleRuns);
  updateRuns(instance.centerCapsuleRuns);
  updateRuns(instance.endCapsuleRuns);
  // animate() only queues; the first tick needs a frame requested here.
  if (changed && instance.surface != nullptr) {
    instance.surface->requestLayout();
  }
}

void Bar::rebuildInstanceContents(BarInstance& instance, const BarConfig& newConfig) {
  noctalia::profiling::ScopedTimer t(kLog, std::format("bar: rebuild contents [{}]", newConfig.name));

  const bool restartHideDelay =
      instance.autoHideDelayTimer.active() && instance.barConfig.autoHideDelayMs != newConfig.autoHideDelayMs;
  if (restartHideDelay) {
    instance.autoHideDelayTimer.stop();
  }

  // Drop any pointer hover/capture state pointing into the widgets we're about
  // to destroy. Hover will be re-acquired on the next pointer motion.
  instance.inputDispatcher.pointerLeave();

  instance.barConfig = newConfig;

  // Detach old widget root nodes from their sections and destroy the widgets.
  // Widgets release their root into the section on creation, so the section
  // owns those nodes — clearing the section frees the scene tree.
  auto clearChildren = [](Node* node) {
    if (node == nullptr) {
      return;
    }
    while (!node->children().empty()) {
      node->removeChild(node->children().back().get());
    }
  };
  clearChildren(instance.startSection);
  clearChildren(instance.centerSection);
  clearChildren(instance.endSection);
  instance.startWidgets.clear();
  instance.centerWidgets.clear();
  instance.endWidgets.clear();
  instance.startCapsuleRuns.clear();
  instance.centerCapsuleRuns.clear();
  instance.endCapsuleRuns.clear();

  // Refresh section-level layout knobs that may have changed (gap; direction
  // doesn't change because position is part of the surface-fields gate).
  const auto widgetSpacing = static_cast<float>(instance.barConfig.widgetSpacing);
  if (instance.startSection != nullptr) {
    instance.startSection->setGap(widgetSpacing);
  }
  if (instance.centerSection != nullptr) {
    instance.centerSection->setVisible(!(instance.barConfig.islands && m_centerIslandHiddenOutputs.contains(instance.output)));
    instance.centerSection->setGap(widgetSpacing);
  }
  if (instance.endSection != nullptr) {
    instance.endSection->setGap(widgetSpacing);
  }

  populateWidgets(instance);
  attachWidgetsToSections(instance);

  applyBackgroundPalette(instance);
  syncBarSurfaceChrome(instance);

  if (instance.surface != nullptr) {
    // Re-run buildScene at the current surface size so radii / styling pick
    // up changes. The first-frame branch is skipped because sceneRoot is
    // already in place.
    const auto w = instance.surface->width();
    const auto h = instance.surface->height();
    if (w > 0 && h > 0) {
      buildScene(instance, w, h);
    }
    instance.surface->requestLayout();
  }
  if (restartHideDelay
      && !instance.pointerInside
      && instance.attachedPopupCount == 0
      && barPointerHideAllowed(instance)) {
    startHideFadeOut(instance);
  }
}

void Bar::tickWidgets(std::vector<std::unique_ptr<Widget>>& widgets, float deltaMs) { ::tickWidgets(widgets, deltaMs); }

bool Bar::widgetsNeedFrameTick(const std::vector<std::unique_ptr<Widget>>& widgets) {
  return ::widgetsNeedFrameTick(widgets);
}

bool Bar::instanceNeedsFrameTick(const BarInstance& instance) {
  return widgetsNeedFrameTick(instance.startWidgets)
      || widgetsNeedFrameTick(instance.centerWidgets)
      || widgetsNeedFrameTick(instance.endWidgets);
}

bool Bar::widgetsWantSecondTicks(const std::vector<std::unique_ptr<Widget>>& widgets) {
  return ::widgetsWantSecondTicks(widgets);
}

bool Bar::instanceWantsSecondTicks(const BarInstance& instance) {
  return widgetsWantSecondTicks(instance.startWidgets)
      || widgetsWantSecondTicks(instance.centerWidgets)
      || widgetsWantSecondTicks(instance.endWidgets);
}

bool Bar::wantsSecondTicks() const {
  return std::ranges::any_of(m_instances, [](const auto& instance) {
    return instance != nullptr && instanceWantsSecondTicks(*instance);
  });
}

