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

  std::pair<float, float> surfaceOriginForOutputLocal(const BarInstance& instance, const WaylandOutput& outputInfo) {
    if (instance.surface == nullptr) {
      return {0.0F, 0.0F};
    }
    const auto* surface = instance.surface.get();
    const std::uint32_t anchor = surface->anchor();
    const bool aTop = (anchor & LayerShellAnchor::Top) != 0;
    const bool aBottom = (anchor & LayerShellAnchor::Bottom) != 0;
    const bool aLeft = (anchor & LayerShellAnchor::Left) != 0;
    const bool aRight = (anchor & LayerShellAnchor::Right) != 0;
    const auto mTop = static_cast<float>(surface->marginTop());
    const auto mRight = static_cast<float>(surface->marginRight());
    const auto mBottom = static_cast<float>(surface->marginBottom());
    const auto mLeft = static_cast<float>(surface->marginLeft());
    const auto surfW = static_cast<float>(surface->width());
    const auto surfH = static_cast<float>(surface->height());
    const auto outputW = static_cast<float>(outputInfo.effectiveLogicalWidth());
    const auto outputH = static_cast<float>(outputInfo.effectiveLogicalHeight());

    float x = mLeft;
    float y = mTop;
    if (aRight && !aLeft) {
      x = std::max(0.0F, outputW - mRight - surfW);
    }

    if (aBottom && !aTop) {
      y = std::max(0.0F, outputH - mBottom - surfH);
    }
    return {x, y};
  }


} // namespace

namespace {

  bool isBarDeadZone(const BarInstance& instance, float sceneX, float sceneY) {
    if (widgetAtPoint(instance, sceneX, sceneY) != nullptr) {
      return false;
    }
    return pointInsideNode(instance.startSection, sceneX, sceneY)
        || pointInsideNode(instance.centerSection, sceneX, sceneY)
        || pointInsideNode(instance.endSection, sceneX, sceneY)
        || pointInsideNode(instance.sceneRoot.get(), sceneX, sceneY);
  }


} // namespace

namespace bar_detail {

  // Bar tooltips stay suppressed while any panel opened from the bar is alive.
  void suppressTooltipForOpenPanel(std::string_view panelId) {
    TooltipManager::instance().suppressBarTooltipsForPanel(panelId);
  }

  // The visible island under bar-surface point sx, sy, as x, y, width, height in the bar surface.
  std::optional<std::array<float, 4>> islandRectAt(const BarInstance& instance, float sx, float sy) {
    if (!instance.barConfig.islands) {
      return std::nullopt;
    }
    for (const Box* island : instance.islands) {
      if (island == nullptr || !island->visible()) {
        continue;
      }
      float x = 0.0F;
      float y = 0.0F;
      Node::absolutePosition(island, x, y);
      if (sx >= x && sx < x + island->width() && sy >= y && sy < y + island->height()) {
        return std::array<float, 4>{x, y, island->width(), island->height()};
      }
    }
    return std::nullopt;
  }


} // namespace

namespace {

  // The dead zone has no widget to anchor to, so a panel action anchors at the pointer instead.
  void openPanelAtBarPointer(
      BarInstance& instance, float sx, float sy, CompositorPlatform* platform, std::string_view sourceBarName,
      std::string_view panelId, std::string_view context, bool toggle
  ) {
    auto& panelManager = PanelManager::instance();
    if (toggle && panelManager.isOpenPanel(panelId)) {
      panelManager.closePanel();
      return;
    }

    float anchorX = sx;
    float anchorY = sy;
    if (platform != nullptr && instance.output != nullptr) {
      if (const auto* out = platform->findOutputByWl(instance.output); out != nullptr && out->hasUsableGeometry()) {
        const auto [surfaceX, surfaceY] = surfaceOriginForOutputLocal(instance, *out);
        anchorX += surfaceX;
        anchorY += surfaceY;
      }
    }
    panelManager.openPanel(
        std::string(panelId),
        PanelOpenRequest{
            .output = instance.output,
            .anchorX = anchorX,
            .anchorY = anchorY,
            .hasAnchorPosition = true,
            .context = context,
            .sourceBarName = sourceBarName,
        }
    );
  }

  bool dispatchBarDeadZoneGesture(
      BarInstance& instance, noctalia::bar::Gesture gesture, float sx, float sy, CompositorPlatform* platform,
      const noctalia::bar::WidgetActionDispatcher& dispatcher
  ) {
    const auto* action = instance.deadZoneBindings.find(gesture);
    if (action == nullptr) {
      return false;
    }

    if (action->kind == noctalia::bar::WidgetAction::Kind::Ipc && noctalia::bar::isAnchoredPanelVerb(action->verb)) {
      const auto args = noctalia::bar::parsePanelVerbArgs(action->args);
      if (args.panelId.empty()) {
        kLog.error(
            "bar.{}.dead_zone.actions.{}: \"{}\" needs a panel id", instance.barConfig.name, gestureConfigKey(gesture),
            action->verb
        );
        return false;
      }
      openPanelAtBarPointer(
          instance, sx, sy, platform, instance.barConfig.name, args.panelId, args.panelContext,
          action->verb == "panel-toggle"
      );
      return true;
    }

    // The dispatcher reports the command and the failure itself; repeating it here would double
    // every line on a held scroll.
    return dispatcher.run(*action, IpcInvocationContext{.barName = instance.barConfig.name, .output = instance.output});
  }

  bool handleBarDeadZoneButton(
      BarInstance& instance, float sx, float sy, std::uint32_t button, CompositorPlatform* platform,
      const noctalia::bar::WidgetActionDispatcher& dispatcher
  ) {
    if (!isBarDeadZone(instance, sx, sy)) {
      return false;
    }
    const auto gesture = noctalia::bar::gestureForButton(button);
    if (!gesture.has_value()) {
      return false;
    }
    return dispatchBarDeadZoneGesture(instance, *gesture, sx, sy, platform, dispatcher);
  }

  bool handleBarDeadZoneAxis(
      BarInstance& instance, float sx, float sy, const PointerEvent& event, CompositorPlatform* platform,
      const noctalia::bar::WidgetActionDispatcher& dispatcher
  ) {
    if (!isBarDeadZone(instance, sx, sy)) {
      return false;
    }

    // Routed through a scene-less InputArea for detent accumulation. Cycle actions fire once per
    // gesture; other actions fire for every step, matching a widget's `scroll_repeat = "auto"`.
    instance.deadZoneAxisSink.setOnAxisHandler([&](const InputArea::PointerData& data) {
      const auto gesture = noctalia::bar::gestureForScroll(data.axis, data.scrollSteps());
      if (!gesture.has_value()) {
        return false;
      }
      const auto* action = instance.deadZoneBindings.find(*gesture);
      if (!data.scrollStepStartsGesture() && action != nullptr && dispatcher.cycles(*action)) {
        return true;
      }
      return dispatchBarDeadZoneGesture(instance, *gesture, sx, sy, platform, dispatcher);
    });
    return instance.deadZoneAxisSink.dispatchAxis(
        sx, sy, event.axis, event.axisSource, event.axisValue, event.axisDiscrete, event.axisValue120, event.axisLines,
        event.axisGestureSerial
    );
  }


  // Island content inset from its pill edge; the bar's main-axis padding equals it so the pill's outer edge sits at
  // the bar's end margin.
  float barIslandInset(const BarConfig& config) {
    return std::round(Tokens::componentIslandInsetText * config.scale);
  }

  // R5: symbol ends 8, text ends 12; the workspace track ends the left island at cell inset 4.
  struct IslandInsets {
    float start;
    float end;
  };

  IslandInsets barIslandInsets(const BarConfig& config, std::size_t island) {
    const float symbol = std::round(Tokens::componentIslandInsetSymbol * config.scale);
    const float text = barIslandInset(config);
    const float track = std::round(Tokens::componentIslandCellInset * config.scale);
    switch (island) {
    case 0:
      return {symbol, track};
    case 2:
      return {symbol, text};
    default:
      return {text, text};
    }
  }


} // namespace

namespace bar_detail {

  float barMainPadding(const BarConfig& config) {
    return config.islands ? barIslandInsets(config, 0).start : static_cast<float>(config.padding);
  }

  float barMainPaddingEnd(const BarConfig& config) {
    return config.islands ? barIslandInsets(config, 2).end : static_cast<float>(config.padding);
  }

  // Each island spans the bar's thickness and its section's extent plus an inset of 0.3 x thickness.
  void layoutBarIslands(BarInstance& instance, bool isVertical) {
    if (!instance.barConfig.islands || instance.bg == nullptr || instance.slideRoot == nullptr) {
      return;
    }
    float rootX = 0.0F;
    float rootY = 0.0F;
    Node::absolutePosition(instance.slideRoot, rootX, rootY);
    const float cross = isVertical ? instance.bg->width() : instance.bg->height();
    const std::array<Flex*, 3> sections{instance.startSection, instance.centerSection, instance.endSection};
    for (std::size_t i = 0; i < sections.size(); ++i) {
      Box* island = instance.islands[i];
      const Flex* section = sections[i];
      if (island == nullptr) {
        continue;
      }
      const float main = section == nullptr ? 0.0F : (isVertical ? section->height() : section->width());
      const bool shown = section != nullptr && section->visible() && main > 0.5F;
      island->setVisible(shown);
      Box* shadow = instance.islandShadows[i];
      Box* edge = instance.islandEdges[i];
      if (shadow != nullptr) {
        shadow->setVisible(shown);
      }
      if (edge != nullptr) {
        edge->setVisible(shown);
      }
      if (!shown) {
        continue;
      }
      float x = 0.0F;
      float y = 0.0F;
      Node::absolutePosition(section, x, y);
      const IslandInsets inset = barIslandInsets(instance.barConfig, i);
      const float w = isVertical ? cross : main + inset.start + inset.end;
      const float h = isVertical ? main + inset.start + inset.end : cross;
      island->setPosition(
          isVertical ? instance.bg->x() : x - rootX - inset.start,
          isVertical ? y - rootY - inset.start : instance.bg->y()
      );
      island->setSize(w, h);
      island->setRadius(std::min(w, h) * 0.5F);
      if (shadow != nullptr && instance.islandShadowStyle.has_value()) {
        shadow->setStyle(*instance.islandShadowStyle);
        shadow->setPosition(island->x(), island->y());
        shadow->setSize(w, h);
        shadow->setRadius(std::min(w, h) * 0.5F);
      }
      if (edge != nullptr) {
        const float r = std::min(w, h) * 0.5F;
        edge->setPosition(island->x() + r * 0.55F, island->y());
        edge->setSize(std::max(0.0F, w - r * 1.1F), kIslandStrokeWidth);
        edge->setStyle(RoundedRectStyle{.fill = islandEdgeColor(), .radius = Radii{kIslandStrokeWidth * 0.5F}});
      }
    }
  }


} // namespace

void Bar::syncSheetTracker(BarInstance& instance) {
  dsk_surface_manager_v1* manager = m_platform != nullptr ? m_platform->wayland().dskSurfaceManager() : nullptr;
  Box* island = instance.islands[2];
  if (manager == nullptr || instance.surface == nullptr || instance.surface->wlSurface() == nullptr
      || !instance.barConfig.islands || instance.barConfig.position != "top" || island == nullptr || !island->visible()) {
    instance.sheetTracker.reset();
    return;
  }
  float x = 0.0F;
  float y = 0.0F;
  Node::absolutePosition(island, x, y);
  const std::array<float, 4> rect{x, y, island->width(), island->height()};
  if (instance.sheetTracker != nullptr && rect == instance.sheetTrackerRect) {
    return;
  }
  if (instance.sheetTracker == nullptr) {
    BarInstance* inst = &instance;
    instance.sheetTracker = std::make_shared<DskTrackerClient>(
        manager, instance.surface->wlSurface(),
        DskTrackerClient::Callbacks{
            .interacting = [this, inst] { openSheetFromTracker(*inst); },
            .idle = [this, inst](double position) { finishSheetTracker(*inst, position); },
        }
    );
    instance.sheetTracker->setRange(0.0, Tokens::componentSheetOpenDrag);
    instance.sheetTracker->addSnap(0.0);
    instance.sheetTracker->addSnap(Tokens::componentSheetOpenDrag);
  }
  wl_region* region = wl_compositor_create_region(m_platform->wayland().compositor());
  wl_region_add(
      region, static_cast<std::int32_t>(std::lround(x)), static_cast<std::int32_t>(std::lround(y)),
      static_cast<std::int32_t>(std::lround(rect[2])), static_cast<std::int32_t>(std::lround(rect[3]))
  );
  instance.sheetTracker->setSource(DskTrackerClient::Axis::Vertical, 0, region);
  wl_region_destroy(region);
  instance.sheetTrackerRect = rect;
  // Tracker state applies on the bar surface's next commit.
  instance.surface->requestRedraw();
}

void Bar::openSheetFromTracker(BarInstance& instance) {
  constexpr std::string_view kPanel = "control-center";
  if (PanelManager::instance().isOpenPanel(kPanel) || instance.sheetTracker == nullptr) {
    return;
  }
  const auto& r = instance.sheetTrackerRect;
  float anchorX = r[0] + r[2] * 0.5F;
  float anchorY = r[1] + r[3] * 0.5F;
  if (m_platform != nullptr && instance.output != nullptr) {
    if (const auto* out = m_platform->findOutputByWl(instance.output); out != nullptr && out->hasUsableGeometry()) {
      const auto [surfaceX, surfaceY] = surfaceOriginForOutputLocal(instance, *out);
      anchorX += surfaceX;
      anchorY += surfaceY;
    }
  }
  std::weak_ptr<DskTrackerClient> tracker = instance.sheetTracker;
  BarInstance* inst = &instance;
  PanelManager::instance().openPanel(
      std::string(kPanel),
      PanelOpenRequest{
          .output = instance.output,
          .anchorX = anchorX,
          .anchorY = anchorY,
          .hasAnchorPosition = true,
          .context = "quick",
          .sourceBarName = instance.barConfig.name,
          .sourceSurface = instance.surface->wlSurface(),
          .sourceRect = r,
          .tracker = tracker,
          .trackerRange = Tokens::componentSheetOpenDrag,
          // The tracker lives on this bar's surface, so the tracker being alive means `inst` is too.
          .commitTracker =
              [tracker, inst] {
                if (!tracker.expired() && inst->surface != nullptr) {
                  inst->surface->requestRedraw();
                }
              },
      }
  );
}

void Bar::finishSheetTracker(BarInstance& instance, double position) {
  PanelManager::instance().finishTrackerOpen(position >= Tokens::componentSheetOpenDrag * 0.5);
  // A tracker keeps its bindings; a new one starts the next drag clean. Not from inside its own callback.
  std::weak_ptr<DskTrackerClient> tracker = instance.sheetTracker;
  BarInstance* inst = &instance;
  DeferredCall::callLater([tracker, inst] {
    if (tracker.expired()) {
      return;
    }
    inst->sheetTracker.reset();
    if (inst->surface != nullptr) {
      inst->surface->requestRedraw();
    }
  });
}

bool Bar::onPointerEvent(const PointerEvent& event) {
  bool consumed = false;
  BarInstance* targetInstance = nullptr;
  if (event.surface != nullptr) {
    targetInstance = instanceForSurface(event.surface);
  } else {
    targetInstance = m_hoveredInstance;
  }

  auto routeWidgetPopups = [&](BarInstance& instance) {
    auto routeGroup = [&](std::vector<std::unique_ptr<Widget>>& widgets) {
      for (auto& widget : widgets) {
        if (widget != nullptr && widget->onPointerEvent(event)) {
          return true;
        }
      }
      return false;
    };
    return routeGroup(instance.startWidgets) || routeGroup(instance.centerWidgets) || routeGroup(instance.endWidgets);
  };
  if (targetInstance != nullptr) {
    if (!instanceAcceptsPointerInput(*targetInstance)) {
      clearInstancePointerState(*targetInstance);
      return false;
    }
    if (routeWidgetPopups(*targetInstance)) {
      return true;
    }
  } else {
    for (const auto& instance : m_instances) {
      if (instance != nullptr && instanceAcceptsPointerInput(*instance) && routeWidgetPopups(*instance)) {
        return true;
      }
    }
  }

  if (targetInstance != nullptr && targetInstance->attachedPopupCount > 0) {
    switch (event.type) {
    case PointerEvent::Type::Enter:
      m_hoveredInstance = targetInstance;
      targetInstance->pointerInside = true;
      break;
    case PointerEvent::Type::Leave:
      targetInstance->pointerInside = false;
      if (m_hoveredInstance == targetInstance) {
        m_hoveredInstance = nullptr;
      }
      break;
    case PointerEvent::Type::Motion:
    case PointerEvent::Type::Button:
    case PointerEvent::Type::Axis:
      if (event.type == PointerEvent::Type::Button && event.button == BTN_RIGHT && event.pressed) {
        const auto sx = static_cast<float>(event.sx);
        const auto sy = static_cast<float>(event.sy);
        if (!handleBarDeadZoneButton(*targetInstance, sx, sy, event.button, m_platform, m_actionDispatcher)) {
          openPanelAtBarPointer(
              *targetInstance, sx, sy, m_platform, targetInstance->barConfig.name, "control-center", "home", true
          );
        }
        return true;
      }
      break;
    }
    return false;
  }

  switch (event.type) {
  case PointerEvent::Type::Enter: {
    auto it = m_surfaceMap.find(event.surface);
    if (it == m_surfaceMap.end()) {
      break;
    }
    m_hoveredInstance = it->second;
    BarInstance* const entered = m_hoveredInstance;
    entered->lastPointerSx = static_cast<float>(event.sx);
    entered->lastPointerSy = static_cast<float>(event.sy);
    entered->pointerInside = true;
    entered->inputDispatcher.pointerEnter(static_cast<float>(event.sx), static_cast<float>(event.sy), event.serial);
    // pointerEnter can re-enter the Wayland event loop (tooltip popup work),
    // which may clear or change m_hoveredInstance before we dereference it.
    if (m_hoveredInstance != entered) {
      break;
    }
    if (barSupportsSlideBehavior(m_hoveredInstance->barConfig) && m_hoveredInstance->sceneRoot != nullptr) {
      revealAutoHideBar(*m_hoveredInstance);
    }
    break;
  }
  case PointerEvent::Type::Leave: {
    if (m_hoveredInstance != nullptr) {
      m_hoveredInstance->pointerInside = false;
      m_hoveredInstance->inputDispatcher.pointerLeave();
      updateAccordionExpansion(*m_hoveredInstance, nullptr);
      const bool suppressAutoHide =
          (m_autoHideSuppressionCallback != nullptr) ? m_autoHideSuppressionCallback(*m_hoveredInstance) : false;
      if (barPointerHideAllowed(*m_hoveredInstance) && !suppressAutoHide) {
        startHideFadeOut(*m_hoveredInstance);
      }
      m_hoveredInstance = nullptr;
    }
    break;
  }
  case PointerEvent::Type::Motion: {
    if (m_hoveredInstance == nullptr)
      break;
    BarInstance* const hovered = m_hoveredInstance;
    hovered->lastPointerSx = static_cast<float>(event.sx);
    hovered->lastPointerSy = static_cast<float>(event.sy);
    hovered->inputDispatcher.pointerMotion(static_cast<float>(event.sx), static_cast<float>(event.sy), 0);
    // pointerMotion can re-enter the Wayland event loop (tooltip popup work),
    // which may clear or change m_hoveredInstance before we dereference it.
    if (m_hoveredInstance != hovered) {
      break;
    }
    updateAccordionExpansion(*hovered, hovered->inputDispatcher.hoveredArea());
    break;
  }
  case PointerEvent::Type::Button: {
    if (m_hoveredInstance == nullptr)
      break;
    m_hoveredInstance->lastPointerSx = static_cast<float>(event.sx);
    m_hoveredInstance->lastPointerSy = static_cast<float>(event.sy);
    const auto sx = static_cast<float>(event.sx);
    const auto sy = static_cast<float>(event.sy);
    bool pressed = event.pressed;
    consumed = m_hoveredInstance->inputDispatcher.pointerButton(
        sx, sy, event.button, pressed, event.serial, event.time, event.touch
    );
    if (pressed
        && !consumed
        && handleBarDeadZoneButton(*m_hoveredInstance, sx, sy, event.button, m_platform, m_actionDispatcher)) {
      consumed = true;
    }

    break;
  }
  case PointerEvent::Type::Axis: {
    if (m_hoveredInstance == nullptr)
      break;
    m_hoveredInstance->lastPointerSx = static_cast<float>(event.sx);
    m_hoveredInstance->lastPointerSy = static_cast<float>(event.sy);
    const auto sx = static_cast<float>(event.sx);
    const auto sy = static_cast<float>(event.sy);
    const bool axisConsumed = m_hoveredInstance->inputDispatcher.pointerAxis(
        sx, sy, event.axis, event.axisSource, event.axisValue, event.axisDiscrete, event.axisValue120, event.axisLines,
        event.axisGestureSerial
    );
    if (!axisConsumed) {
      handleBarDeadZoneAxis(*m_hoveredInstance, sx, sy, event, m_platform, m_actionDispatcher);
    }
    break;
  }
  }

  // Trigger redraw if any widget changed visual state
  if (m_hoveredInstance != nullptr
      && m_hoveredInstance->sceneRoot != nullptr
      && (m_hoveredInstance->sceneRoot->paintDirty() || m_hoveredInstance->sceneRoot->layoutDirty())) {
    if (m_hoveredInstance->sceneRoot->layoutDirty()) {
      m_hoveredInstance->surface->requestLayout();
    } else {
      m_hoveredInstance->surface->requestRedraw();
    }
  }

  return consumed;
}

