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

namespace {

  constexpr std::chrono::milliseconds kWorkspaceRevealDebounce{80};
  constexpr std::chrono::milliseconds kWorkspacePeekHold{450};

} // namespace

namespace bar_detail {

  [[nodiscard]] std::string activeWorkspaceId(const std::vector<Workspace>& workspaces) {
    for (const auto& workspace : workspaces) {
      if (workspace.active) {
        if (!workspace.id.empty()) {
          return workspace.id;
        }
        if (!workspace.name.empty()) {
          return workspace.name;
        }
        return std::to_string(workspace.index);
      }
    }
    return {};
  }


} // namespace

namespace {

  [[nodiscard]] bool workspaceKeyMatchesAssignment(std::string_view assignmentKey, const Workspace& workspace) {
    if (assignmentKey.empty()) {
      return false;
    }
    if (!workspace.id.empty() && assignmentKey == workspace.id) {
      return true;
    }
    if (!workspace.name.empty() && assignmentKey == workspace.name) {
      return true;
    }
    if (workspace.index > 0 && assignmentKey == std::to_string(workspace.index)) {
      return true;
    }
    return false;
  }

  [[nodiscard]] bool activeWorkspaceHasWindows(const CompositorPlatform& platform, wl_output* output) {
    const auto workspaces = platform.workspaces(output);
    const Workspace* active = nullptr;
    for (const auto& workspace : workspaces) {
      if (workspace.active) {
        active = &workspace;
        break;
      }
    }
    if (active == nullptr) {
      return false;
    }

    const auto assignments = platform.workspaceWindowAssignments(output);
    for (const auto& assignment : assignments) {
      if (assignment.minimizeState.has_value() && assignment.minimizeState->minimized) {
        continue;
      }
      if (workspaceKeyMatchesAssignment(assignment.workspaceKey, *active)) {
        return true;
      }
    }
    if (!assignments.empty()) {
      return false;
    }
    if (platform.hasUnminimizedToplevels(output) == false) {
      return false;
    }
    return active->occupied;
  }


} // namespace

namespace bar_detail {

  [[nodiscard]] bool smartAutoHideWantsPinnedVisible(const CompositorPlatform& platform, wl_output* output) {
    if (platform.hasOverviewState() && platform.isOverviewOpen()) {
      return true;
    }
    return !activeWorkspaceHasWindows(platform, output);
  }


} // namespace

namespace {

  // Returns true when two bar configs would produce an identical layer-shell
  // surface (same anchor, size, exclusive zone, namespace). When true, an
  // existing BarInstance can be retained on reload and only its widget tree
  // rebuilt — avoiding the screen-shift caused by destroying and recreating
  // the exclusive zone.
  bool barConfigSurfaceFieldsEqual(
      const BarConfig& a, const BarConfig& b, const ShellConfig::ShadowConfig& previousShadow,
      const ShellConfig::ShadowConfig& nextShadow
  ) {
    const bool sameShadowSurface =
        (!a.shadow && !b.shadow) || ui::surface_shadow::sameSurfaceMetrics(previousShadow, nextShadow);
    return a.name == b.name
        && a.position == b.position
        && a.enabled == b.enabled
        && a.autoHide == b.autoHide
        && a.smartAutoHide == b.smartAutoHide
        && a.reserveSpace == b.reserveSpace
        && a.layer == b.layer
        && a.thickness == b.thickness
        // Corner radii feed the concave-corner bulge, which changes the surface size.
        && a.radiusTopLeft == b.radiusTopLeft
        && a.radiusTopRight == b.radiusTopRight
        && a.radiusBottomLeft == b.radiusBottomLeft
        && a.radiusBottomRight == b.radiusBottomRight
        && a.concaveEdgeCorners == b.concaveEdgeCorners
        && a.marginEnds == b.marginEnds
        && a.marginEdge == b.marginEdge
        && a.shadow == b.shadow
        && sameShadowSurface
        && a.monitorOverrides == b.monitorOverrides;
  }

  bool barSurfaceOrderRequiresRecreate(const std::vector<BarConfig>& previous, const std::vector<BarConfig>& next) {
    std::vector<std::string> preserved;
    preserved.reserve(previous.size());
    for (const auto& oldBar : previous) {
      const auto it = std::ranges::find(next, oldBar.name, &BarConfig::name);
      if (it != next.end()) {
        preserved.push_back(oldBar.name);
      }
    }

    if (preserved.size() > next.size()) {
      return true;
    }
    for (std::size_t i = 0; i < preserved.size(); ++i) {
      if (next[i].name != preserved[i]) {
        return true;
      }
    }
    return false;
  }


} // namespace


Bar::Bar() = default;

bool Bar::initialize(const BarServices& services) {
  m_platform = &services.platform;
  m_config = &services.config;
  m_notifications = services.notifications;
  m_tray = services.tray;
  m_audio = services.audio;
  m_easyEffects = services.easyEffects;
  m_upower = services.upower;
  m_sysmon = services.sysmon;
  m_powerProfiles = services.powerProfiles;
  m_network = services.network;
  m_modem = services.modem;
  m_externalIp = services.externalIp;
  m_idleInhibitor = services.idleInhibitor;
  m_mpris = services.mpris;
  m_audioSpectrum = services.audioSpectrum;
  m_httpClient = services.httpClient;
  m_weatherService = services.weather;
  m_renderContext = services.renderContext;
  m_nightLight = services.nightLight;
  m_themeService = services.theme;
  m_bluetooth = services.bluetooth;
  m_link = services.link;
  m_brightness = services.brightness;
  m_lockKeys = services.lockKeys;
  m_clipboard = services.clipboard;
  m_fileWatcher = services.fileWatcher;
  m_screenshots = services.screenshots;
  m_scriptApi = services.scriptApi;

  m_widgetFactory = std::make_unique<WidgetFactory>(services);

  m_lastBars = m_config->config().bars;
  m_lastWidgets = m_config->config().widgets;
  m_lastShadow = m_config->config().shell.shadow;
  m_lastPlugins = m_config->config().plugins;
  m_config->addReloadCallback(
      [this] {
        const auto& cfg = m_config->config();
        if (cfg.bars == m_lastBars
            && cfg.widgets == m_lastWidgets
            && cfg.shell.shadow == m_lastShadow
            && cfg.plugins == m_lastPlugins) {
          return;
        }
        reload();
      },
      "bar"
  );

  return true;
}

BarServices Bar::services() const {
  return {
      .platform = *m_platform,
      .config = *m_config,
      .notifications = m_notifications,
      .tray = m_tray,
      .audio = m_audio,
      .easyEffects = m_easyEffects,
      .upower = m_upower,
      .sysmon = m_sysmon,
      .powerProfiles = m_powerProfiles,
      .network = m_network,
      .modem = m_modem,
      .externalIp = m_externalIp,
      .idleInhibitor = m_idleInhibitor,
      .mpris = m_mpris,
      .audioSpectrum = m_audioSpectrum,
      .httpClient = m_httpClient,
      .weather = m_weatherService,
      .renderContext = m_renderContext,
      .nightLight = m_nightLight,
      .theme = m_themeService,
      .bluetooth = m_bluetooth,
      .link = m_link,
      .brightness = m_brightness,
      .lockKeys = m_lockKeys,
      .clipboard = m_clipboard,
      .fileWatcher = m_fileWatcher,
      .screenshots = m_screenshots,
      .scriptApi = m_scriptApi,
  };
}

void Bar::onSecondTick() {
  // Without a widget that renders seconds, a full bar update once a minute is
  // enough, so skip the other 59 ticks instead of re-laying out every surface.
  const auto minute = std::chrono::floor<std::chrono::minutes>(std::chrono::system_clock::now());
  const bool minuteChanged = minute != m_lastTickMinute;
  m_lastTickMinute = minute;
  for (auto& inst : m_instances) {
    if (inst->surface == nullptr) {
      continue;
    }
    if (!minuteChanged && !instanceWantsSecondTicks(*inst)) {
      continue;
    }
    inst->surface->requestUpdate();
  }
}

void Bar::reload() {
  noctalia::profiling::ScopedTimer t(kLog, "bar: reload (all instances)");
  kLog.info("reloading config");
  const auto previousBars = m_lastBars;
  const auto previousShadow = m_lastShadow;
  const bool recreateForOrder = barSurfaceOrderRequiresRecreate(previousBars, m_config->config().bars);
  m_lastBars = m_config->config().bars;
  m_lastWidgets = m_config->config().widgets;
  m_lastShadow = m_config->config().shell.shadow;
  m_lastPlugins = m_config->config().plugins;
  m_widgetFactory = std::make_unique<WidgetFactory>(services());

  if (recreateForOrder) {
    kLog.info("bar order changed; recreating layer-shell surfaces");
    closeAllInstances();
    if (wl_display_roundtrip(m_platform->display()) < 0) {
      const int roundtripErrno = errno;
      kLog.error(
          "Wayland roundtrip failed after destroying bar surfaces for order change: {}",
          m_platform->wayland().describeDisplayError(roundtripErrno)
      );
    }
    syncInstances();
    return;
  }

  // Look up new bar configs by name.
  std::unordered_map<std::string, std::pair<const BarConfig*, std::size_t>> newBarsByName;
  newBarsByName.reserve(m_lastBars.size());
  for (std::size_t i = 0; i < m_lastBars.size(); ++i) {
    newBarsByName[m_lastBars[i].name] = {&m_lastBars[i], i};
  }

  // Exclusive-zone geometry on an output depends on the order its bar surfaces are
  // created: bars on the same edge stack in creation order, and bars on adjacent
  // edges (e.g. top + left) compete for the shared corner the same way. Rebuilding
  // one bar's surface in place while recreating another would commit them out of
  // config order and reshuffle that geometry. So if any bar on an output needs a
  // surface recreate, recreate every bar on that output — syncInstances rebuilds
  // them in config order. Scoped per output so other monitors are untouched.
  const auto needsSurfaceRecreate = [&](const BarInstance& inst) -> bool {
    auto it = newBarsByName.find(inst.barConfig.name);
    if (it == newBarsByName.end()) {
      return true;
    }
    const auto& outputs = m_platform->outputs();
    auto outIt = std::ranges::find(outputs, inst.outputName, &WaylandOutput::name);
    if (outIt == outputs.end()) {
      return true;
    }
    auto resolved = ConfigService::resolveForOutput(*it->second.first, *outIt);
    if (!resolved.enabled) {
      return true;
    }
    return !barConfigSurfaceFieldsEqual(inst.barConfig, resolved, previousShadow, m_lastShadow);
  };
  std::unordered_set<std::uint32_t> outputsNeedingRecreate;
  for (const auto& instUp : m_instances) {
    if (needsSurfaceRecreate(*instUp)) {
      outputsNeedingRecreate.insert(instUp->outputName);
    }
  }

  // For each existing instance, decide whether to rebuild contents in place
  // (surface preserved → no exclusive-zone churn) or destroy (will be recreated
  // by syncInstances below). Any bar on an output flagged above is destroyed so the
  // whole output is rebuilt in config order.
  bool destroyedAny = false;
  std::erase_if(m_instances, [&](const std::unique_ptr<BarInstance>& instUp) {
    auto& inst = *instUp;
    auto it = newBarsByName.find(inst.barConfig.name);
    auto destroy = [&] {
      if (inst.surface != nullptr) {
        m_surfaceMap.erase(inst.surface->wlSurface());
      }
      if (m_hoveredInstance == &inst) {
        m_hoveredInstance = nullptr;
      }
      destroyedAny = true;
      return true;
    };
    if (outputsNeedingRecreate.contains(inst.outputName)) {
      return destroy();
    }
    if (it == newBarsByName.end()) {
      return destroy();
    }

    const auto& outputs = m_platform->outputs();
    auto outIt = std::ranges::find(outputs, inst.outputName, &WaylandOutput::name);
    if (outIt == outputs.end()) {
      return destroy();
    }

    auto resolved = ConfigService::resolveForOutput(*it->second.first, *outIt);
    if (!resolved.enabled) {
      return destroy();
    }

    inst.barIndex = it->second.second;
    rebuildInstanceContents(inst, resolved);
    return false;
  });

  // Drain pending Wayland events for the just-destroyed surfaces before
  // creating new ones. Without this, the roundtrip inside LayerSurface::initialize
  // reads stale closures for dead proxies, which libwayland drops without freeing.
  if (destroyedAny && wl_display_roundtrip(m_platform->display()) < 0) {
    const int roundtripErrno = errno;
    kLog.error(
        "Wayland roundtrip failed after destroying stale bar surfaces: {}",
        m_platform->wayland().describeDisplayError(roundtripErrno)
    );
  }

  syncInstances();
}

void Bar::closeAllInstances() {
  m_surfaceMap.clear();
  m_hoveredInstance = nullptr;
  m_instances.clear();
  if (m_centerIslandSpaceCallback) {
    m_centerIslandSpaceCallback();
  }
}

void Bar::onOutputChange() { syncInstances(); }

void Bar::onWorkspaceChanged() {
  if (m_platform == nullptr || m_overlayDisplaySuppressed) {
    return;
  }

  bool anyChanged = false;
  for (const auto& output : m_platform->outputs()) {
    const std::string activeId = activeWorkspaceId(m_platform->workspaces(output.output));
    if (activeId.empty()) {
      continue;
    }

    auto& last = m_lastActiveWorkspaceByOutput[output.name];
    if (!last.empty() && last != activeId) {
      m_pendingWorkspaceRevealOutputs.insert(output.name);
      anyChanged = true;
    }
    last = activeId;
  }

  if (anyChanged) {
    m_workspaceRevealDebounce.start(kWorkspaceRevealDebounce, [this] { applyPendingWorkspaceReveal(); });
  }
  scheduleSmartAutoHideReevaluation();
}

void Bar::scheduleSmartAutoHideReevaluation() {
  if (m_smartAutoHideReevalQueued) {
    return;
  }
  m_smartAutoHideReevalQueued = true;
  DeferredCall::callLater([this] {
    m_smartAutoHideReevalQueued = false;
    reevaluateSmartAutoHide();
  });
}

void Bar::reevaluateSmartAutoHide() {
  if (m_platform == nullptr || m_overlayDisplaySuppressed) {
    return;
  }

  for (const auto& instanceUp : m_instances) {
    BarInstance* instance = instanceUp.get();
    if (instance == nullptr
        || !instance->barConfig.enabled
        || !instance->barConfig.smartAutoHide
        || instance->surface == nullptr) {
      continue;
    }

    const bool wantsPinned = smartAutoHideWantsPinnedVisible(*m_platform, instance->output);
    const bool pinnedChanged = wantsPinned != instance->smartAutoHidePinnedVisible;
    instance->smartAutoHidePinnedVisible = wantsPinned;

    const bool suppressAutoHide =
        (m_autoHideSuppressionCallback != nullptr) ? m_autoHideSuppressionCallback(*instance) : false;

    bool needsRedraw = pinnedChanged;
    if (wantsPinned) {
      if (instance->hideOpacity < 1.0F || pinnedChanged) {
        revealAutoHideBar(*instance);
        syncBarAutoHideInputRegion(*instance);
        syncBarSurfaceChrome(*instance);
        needsRedraw = true;
      }
    } else if (
        !instance->pointerInside
        && instance->attachedPopupCount == 0
        && !suppressAutoHide
        && (instance->hideOpacity > 0.0F || pinnedChanged)
        && (!instance->barConfig.showOnWorkspaceSwitch || !isWorkspacePeekActive())
    ) {
      startHideFadeOut(*instance);
      needsRedraw = true;
    }

    if (needsRedraw && instance->surface != nullptr) {
      instance->surface->requestRedraw();
    }
  }
}

bool Bar::isWorkspacePeekActive() const noexcept {
  return m_workspaceRevealDebounce.active() || m_workspacePeekHideTimer.active();
}

void Bar::applyPendingWorkspaceReveal() {
  if (m_platform == nullptr) {
    return;
  }

  const auto pendingOutputs = std::move(m_pendingWorkspaceRevealOutputs);
  m_pendingWorkspaceRevealOutputs.clear();

  std::vector<BarInstance*> peeked;
  peeked.reserve(m_instances.size());
  for (const std::uint32_t outputName : pendingOutputs) {
    for (const auto& instanceUp : m_instances) {
      auto* instance = instanceUp.get();
      if (instance == nullptr
          || instance->outputName != outputName
          || !instance->barConfig.enabled
          || !instance->barConfig.isAutoHideEnabled()
          || !instance->barConfig.showOnWorkspaceSwitch
          || instance->surface == nullptr) {
        continue;
      }

      revealAutoHideBar(*instance);
      if (instance->pointerInside) {
        continue;
      }
      const bool suppressAutoHide =
          (m_autoHideSuppressionCallback != nullptr) ? m_autoHideSuppressionCallback(*instance) : false;
      if (!suppressAutoHide) {
        peeked.push_back(instance);
      }
    }
  }

  if (peeked.empty()) {
    return;
  }

  m_workspacePeekHideTimer.start(kWorkspacePeekHold, [this, peeked = std::move(peeked)] {
    for (BarInstance* instance : peeked) {
      if (instance == nullptr || !instance->barConfig.isAutoHideEnabled() || instance->pointerInside) {
        continue;
      }
      if (instance->barConfig.smartAutoHide && instance->smartAutoHidePinnedVisible) {
        continue;
      }
      const bool suppressAutoHide =
          (m_autoHideSuppressionCallback != nullptr) ? m_autoHideSuppressionCallback(*instance) : false;
      if (!suppressAutoHide) {
        startHideFadeOut(*instance, true);
      }
    }
  });
}

void Bar::refresh() {
  for (auto& inst : m_instances) {
    if (inst->surface != nullptr) {
      inst->surface->requestUpdate();
      if (inst->animations.hasActive() || instanceNeedsFrameTick(*inst)) {
        inst->surface->requestRedraw();
      }
    }
  }
}

void Bar::requestRedraw() {
  for (auto& inst : m_instances) {
    if (inst->surface != nullptr) {
      inst->surface->requestRedraw();
    }
  }
}

void Bar::requestLayout() {
  for (auto& inst : m_instances) {
    if (inst->surface != nullptr) {
      inst->surface->requestLayout();
    }
  }
}

void Bar::setCenterIslandHidden(wl_output* output, bool hidden) {
  if (hidden ? !m_centerIslandHiddenOutputs.insert(output).second : m_centerIslandHiddenOutputs.erase(output) == 0) {
    return;
  }
  for (auto& inst : m_instances) {
    if (inst->output == output && inst->centerSection != nullptr && inst->barConfig.islands) {
      inst->centerSection->setVisible(!hidden);
    }
  }
  requestLayout();
}

std::optional<CenterIslandSpace> Bar::centerIslandSpace(wl_output* output) const {
  constexpr float kGap = 12.0F;
  for (const auto& instance : m_instances) {
    const BarConfig& cfg = instance->barConfig;
    const bool horizontal = cfg.position == "top" || cfg.position == "bottom";
    if (instance->output != output || !cfg.islands) {
      continue;
    }
    if (!horizontal) {
      if (instance->bg == nullptr || m_platform == nullptr || instance->bg->width() <= 0.0F) {
        return instance->centerSpace;
      }
      const auto& outputs = m_platform->wayland().outputs();
      const auto it = std::ranges::find_if(outputs, [output](const auto& o) { return o.output == output; });
      if (it == outputs.end()) {
        continue;
      }
      instance->centerSpace = CenterIslandSpace{
          .edgeMargin = static_cast<std::int32_t>(Tokens::spaceChromeGap),
          .height = instance->bg->width(),
          .width = std::max(0.0F, static_cast<float>(it->logicalWidth) - 4.0F * (instance->bg->width() + 2.0F * kGap)),
          .restless = true,
          .opacity = instance->hideOpacity,
          .backgroundOpacity = cfg.backgroundOpacity,
          .border = cfg.border,
          .borderWidth = cfg.borderWidth,
      };
      return instance->centerSpace;
    }
    if (instance->bg == nullptr || instance->surface == nullptr || instance->bg->height() <= 0.0F) {
      return instance->centerSpace;
    }
    const float centerX = static_cast<float>(instance->surface->width()) * 0.5F;
    float halfFree = centerX;
    const auto limit = [&](const Box* island, bool isStart) {
      if (island == nullptr || !island->visible()) {
        return;
      }
      float x = 0.0F;
      float y = 0.0F;
      Node::absolutePosition(island, x, y);
      halfFree = std::min(halfFree, isStart ? centerX - (x + island->width()) : x - centerX);
    };
    limit(instance->islands[0], true);
    limit(instance->islands[2], false);
    instance->centerSpace = CenterIslandSpace{
        .edgeMargin = cfg.marginEdge,
        .height = instance->bg->height(),
        .width = std::max(0.0F, 2.0F * (halfFree - kGap)),
        .bottom = cfg.position == "bottom",
        .opacity = instance->hideOpacity,
        .backgroundOpacity = cfg.backgroundOpacity,
        .border = cfg.border,
        .borderWidth = cfg.borderWidth,
    };
    return instance->centerSpace;
  }
  return std::nullopt;
}

void Bar::notifyCenterIslandSpace(BarInstance& instance) {
  const auto space = centerIslandSpace(instance.output);
  std::array<float, 6> key{-1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F};
  if (space.has_value()) {
    key = {space->width, space->height, static_cast<float>(space->edgeMargin), space->opacity,
           space->backgroundOpacity, space->borderWidth};
  }
  if (key == instance.lastCenterSpaceKey) {
    return;
  }
  instance.lastCenterSpaceKey = key;
  if (m_centerIslandSpaceCallback) {
    m_centerIslandSpaceCallback();
  }
}

void Bar::setAutoHideSuppressionCallback(std::function<bool(const BarInstance&)> callback) {
  m_autoHideSuppressionCallback = std::move(callback);
}

void Bar::reevaluateAutoHide() {
  for (const auto& instance : m_instances) {
    if (instance == nullptr
        || !barPointerHideAllowed(*instance)
        || instance->pointerInside
        || instance->attachedPopupCount > 0) {
      continue;
    }
    const bool suppressAutoHide =
        (m_autoHideSuppressionCallback != nullptr) ? m_autoHideSuppressionCallback(*instance) : false;
    if (suppressAutoHide || instance->hideOpacity <= 0.001F) {
      continue;
    }
    startHideFadeOut(*instance);
  }
}

void Bar::rearmTooltipForHoveredWidget() {
  if (m_hoveredInstance == nullptr || !m_hoveredInstance->pointerInside) {
    return;
  }
  auto* hovered = m_hoveredInstance->inputDispatcher.hoveredArea();
  if (hovered == nullptr || m_hoveredInstance->surface == nullptr) {
    return;
  }
  // Same path a real hover takes, so the usual show delay still applies — the
  // tooltip must not blink into existence the instant the panel disappears.
  TooltipManager::instance().onBarHoverChange(
      tooltipAreaForHover(*m_hoveredInstance, hovered), m_hoveredInstance->surface->layerSurface(),
      m_hoveredInstance->output
  );
}

void Bar::reevaluateAutoHideAfterPopup() {
  for (const auto& instance : m_instances) {
    if (instance == nullptr || instance->surface == nullptr) {
      continue;
    }
    wl_surface* const surface = instance->surface->wlSurface();
    instance->pointerInside = m_platform != nullptr
        && surface != nullptr
        && m_platform->hasPointerPosition()
        && m_platform->lastPointerSurface() == surface;
    if (instance->pointerInside) {
      instance->lastPointerSx = static_cast<float>(m_platform->lastPointerX());
      instance->lastPointerSy = static_cast<float>(m_platform->lastPointerY());
      instance->inputDispatcher.pointerEnter(
          instance->lastPointerSx, instance->lastPointerSy, m_platform->lastInputSerial()
      );
      m_hoveredInstance = instance.get();
    } else {
      instance->inputDispatcher.pointerLeave();
      if (m_hoveredInstance == instance.get()) {
        m_hoveredInstance = nullptr;
      }
    }
    instance->surface->requestRedraw();
  }
  reevaluateAutoHide();
}

bool Bar::isRunning() const noexcept {
  return std::ranges::any_of(m_instances, [](const auto& inst) { return inst->surface && inst->surface->isRunning(); });
}

bool Bar::instanceEffectivelyVisible(const BarInstance& instance) const noexcept {
  if (barSupportsSlideBehavior(instance.barConfig)) {
    return instance.hideOpacity > 0.5F;
  }
  return instance.slideRoot == nullptr || instance.hideOpacity > 0.5F;
}

bool Bar::instanceAcceptsPointerInput(const BarInstance& instance) const noexcept {
  return barSupportsSlideBehavior(instance.barConfig) || !instance.ipcLayoutReleased;
}

bool Bar::isVisible() const noexcept {
  return std::ranges::any_of(m_instances, [this](const auto& inst) { return instanceEffectivelyVisible(*inst); });
}

void Bar::clearInstancePointerState(BarInstance& instance) {
  instance.pointerInside = false;
  instance.inputDispatcher.pointerLeave();
  if (m_hoveredInstance == &instance) {
    m_hoveredInstance = nullptr;
  }
}

void Bar::setInstanceIpcVisible(BarInstance& instance, bool visible) {
  if (instance.surface == nullptr) {
    return;
  }
  if (barSupportsSlideBehavior(instance.barConfig)) {
    if (visible) {
      revealAutoHideBar(instance);
    } else {
      startHideFadeOut(instance, true);
    }
    return;
  }
  if (instance.slideRoot == nullptr) {
    return;
  }
  instance.animations.cancelForOwner(instance.slideRoot);
  instance.slideRoot->setOpacity(1.0F);
  if (!visible) {
    clearInstancePointerState(instance);
  }
  const float current = instance.hideOpacity;
  const float target = visible ? 1.0F : 0.0F;
  instance.animations.animateSpring(
      current, target, Style::spring(visible ? Tokens::springMove : Tokens::springClose),
      [inst = &instance, this](float v) {
        inst->hideOpacity = v;
        syncBarSlideLayerTransform(*inst);
        syncBarSurfaceChrome(*inst);
      },
      [inst = &instance, this] {
        syncBarSlideLayerTransform(*inst);
        syncBarAutoHideInputRegion(*inst);
        syncBarSurfaceChrome(*inst);
        if (inst->surface != nullptr) {
          inst->surface->requestRedraw();
        }
      },
      instance.slideRoot
  );
  syncBarAutoHideInputRegion(instance);
  syncBarSurfaceChrome(instance);
  instance.surface->requestRedraw();
}

void Bar::applyIpcVisibility(bool visible) {
  for (const auto& instance : m_instances) {
    if (instance == nullptr) {
      continue;
    }
    setInstanceIpcVisible(*instance, visible);
    syncBarSurfaceChrome(*instance);
  }
  syncIdleInhibitorAnchors();
}

void Bar::syncIdleInhibitorAnchors() {
  if (m_idleInhibitor != nullptr) {
    m_idleInhibitor->resyncAnchorSurfaces();
  }
}

bool Bar::barContentVisuallyShown(const BarInstance& instance) const noexcept {
  constexpr float kShownThreshold = 0.02F;
  if (barSupportsSlideBehavior(instance.barConfig)) {
    return instance.hideOpacity > kShownThreshold;
  }
  return instance.slideRoot == nullptr || instance.hideOpacity > kShownThreshold;
}

bool Bar::shouldReserveExclusiveZone(const BarInstance& instance) const noexcept {
  if (instance.ipcLayoutReleased) {
    return false;
  }
  return instance.barConfig.reserveSpace;
}

void Bar::syncBarExclusiveZone(BarInstance& instance) {
  if (instance.surface == nullptr || m_config == nullptr) {
    return;
  }
  const std::int32_t zone = shouldReserveExclusiveZone(instance)
      ? reservedBarExclusiveZone(instance.barConfig, m_config->config().shell.shadow)
      : 0;
  instance.surface->setExclusiveZone(zone);
}

void Bar::syncBarSurfaceChrome(BarInstance& instance) {
  syncBarExclusiveZone(instance);
  applyBarCompositorBlur(instance);
  notifyCenterIslandSpace(instance);
}

std::optional<LayerPopupParentContext> Bar::popupParentContextForSurface(wl_surface* surface) const noexcept {
  auto* instance = instanceForSurface(surface);
  if (instance == nullptr || instance->surface == nullptr) {
    return std::nullopt;
  }

  auto* layerSurface = instance->surface->layerSurface();
  const auto width = instance->surface->width();
  const auto height = instance->surface->height();
  if (layerSurface == nullptr || width == 0 || height == 0) {
    return std::nullopt;
  }

  return LayerPopupParentContext{
      .surface = instance->surface->wlSurface(),
      .layerSurface = layerSurface,
      .output = instance->output,
      .width = width,
      .height = height,
  };
}

std::optional<LayerPopupParentContext> Bar::preferredPopupParentContext(wl_output* output) const noexcept {
  BarInstance* instance = instanceForOutput(output);
  if (instance == nullptr && !m_instances.empty()) {
    instance = m_instances.front().get();
  }
  return instance != nullptr && instance->surface != nullptr
      ? popupParentContextForSurface(instance->surface->wlSurface())
      : std::nullopt;
}

std::vector<InputRect> Bar::surfaceRectsForOutput(wl_output* output) const {
  std::vector<InputRect> rects;
  if (m_platform == nullptr || output == nullptr) {
    return rects;
  }

  const WaylandOutput* wlOutput = m_platform->findOutputByWl(output);
  if (wlOutput == nullptr) {
    return rects;
  }
  if (!wlOutput->hasUsableGeometry()) {
    return rects;
  }
  const std::int32_t outputW = wlOutput->effectiveLogicalWidth();
  const std::int32_t outputH = wlOutput->effectiveLogicalHeight();

  for (const auto& instance : m_instances) {
    if (instance == nullptr || instance->output != output || instance->surface == nullptr) {
      continue;
    }
    if (!instanceAcceptsPointerInput(*instance)) {
      continue;
    }
    const auto* surface = instance->surface.get();
    const std::uint32_t anchor = surface->anchor();
    const bool aTop = (anchor & LayerShellAnchor::Top) != 0;
    const bool aBottom = (anchor & LayerShellAnchor::Bottom) != 0;
    const bool aLeft = (anchor & LayerShellAnchor::Left) != 0;
    const bool aRight = (anchor & LayerShellAnchor::Right) != 0;
    const std::int32_t mTop = surface->marginTop();
    const std::int32_t mRight = surface->marginRight();
    const std::int32_t mBottom = surface->marginBottom();
    const std::int32_t mLeft = surface->marginLeft();
    // surface->width()/height() may be 0 before configure; fall back to BarConfig
    // thickness so we still publish a sensible exclusion for fresh surfaces.
    const auto surfW = static_cast<std::int32_t>(surface->width());
    const auto surfH = static_cast<std::int32_t>(surface->height());

    std::int32_t rectW = surfW;
    std::int32_t rectH = surfH;
    std::int32_t rectX = 0;
    std::int32_t rectY = 0;

    if (aLeft && aRight) {
      rectW = std::max(0, outputW - mLeft - mRight);
      rectX = mLeft;
    } else if (aRight) {
      rectX = std::max(0, outputW - mRight - rectW);
    } else {
      rectX = mLeft;
    }

    if (aTop && aBottom) {
      rectH = std::max(0, outputH - mTop - mBottom);
      rectY = mTop;
    } else if (aBottom) {
      rectY = std::max(0, outputH - mBottom - rectH);
    } else {
      rectY = mTop;
    }

    if (rectW > 0 && rectH > 0) {
      rects.push_back(InputRect{rectX, rectY, rectW, rectH});
    }
  }

  return rects;
}

std::vector<wl_surface*> Bar::allBarSurfaces() const {
  std::vector<wl_surface*> surfaces;
  surfaces.reserve(m_instances.size());
  for (const auto& instance : m_instances) {
    if (instance != nullptr && instance->surface != nullptr && instanceAcceptsPointerInput(*instance)) {
      if (wl_surface* s = instance->surface->wlSurface(); s != nullptr) {
        surfaces.push_back(s);
      }
    }
  }
  return surfaces;
}

std::vector<wl_surface*> Bar::caffeineAnchorSurfaces() const {
  std::vector<wl_surface*> surfaces;
  surfaces.reserve(m_instances.size());
  for (const auto& instance : m_instances) {
    if (instance == nullptr || instance->surface == nullptr || !instanceAcceptsPointerInput(*instance)) {
      continue;
    }
    if (!barContentVisuallyShown(*instance)) {
      continue;
    }
    if (wl_surface* s = instance->surface->wlSurface(); s != nullptr) {
      surfaces.push_back(s);
    }
  }
  return surfaces;
}

bool Bar::canAttachPanelToBar(wl_output* output, std::string_view barName) const noexcept {
  const BarInstance* instance = instanceForBar(output, barName);
  if (instance == nullptr || instance->surface == nullptr || !instance->barConfig.enabled) {
    return false;
  }
  return barSupportsSlideBehavior(instance->barConfig) || instanceEffectivelyVisible(*instance);
}

std::optional<BarConfig> Bar::configForBar(wl_output* output, std::string_view barName) const {
  const BarInstance* instance = instanceForBar(output, barName);
  if (instance == nullptr || !instance->barConfig.enabled) {
    return std::nullopt;
  }
  return instance->barConfig;
}

void Bar::fillIslandSource(PanelOpenRequest& request, std::string_view panelId) const {
  if (panelId != "control-center") {
    return;
  }
  for (const auto& instance : m_instances) {
    if (instance->output != request.output || instance->sheetTracker == nullptr || instance->surface == nullptr) {
      continue;
    }
    const auto& r = instance->sheetTrackerRect;
    float anchorX = r[0] + r[2] * 0.5F;
    float anchorY = r[1] + r[3] * 0.5F;
    if (m_platform != nullptr) {
      if (const auto* out = m_platform->findOutputByWl(instance->output); out != nullptr && out->hasUsableGeometry()) {
        const auto [surfaceX, surfaceY] = surfaceOriginForOutputLocal(*instance, *out);
        anchorX += surfaceX;
        anchorY += surfaceY;
      }
    }
    request.anchorX = anchorX;
    request.anchorY = anchorY;
    request.hasAnchorPosition = true;
    request.sourceBarName = instance->barConfig.name;
    request.sourceSurface = instance->surface->wlSurface();
    request.sourceRect = r;
    return;
  }
}

LayerShellLayer Bar::highestLayerForOutput(wl_output* output) const noexcept {
  LayerShellLayer highest = LayerShellLayer::Top;
  for (const auto& instance : m_instances) {
    if (instance->output != output || !instance->barConfig.enabled) {
      continue;
    }
    const LayerShellLayer layer = layerShellLayerFromConfig(instance->barConfig.layer);
    if (static_cast<std::uint32_t>(layer) > static_cast<std::uint32_t>(highest)) {
      highest = layer;
    }
  }
  return highest;
}

bool Bar::isAttachedPanelBarSettled(wl_output* output, std::string_view barName) const noexcept {
  const BarInstance* instance = instanceForBar(output, barName);
  if (instance == nullptr || !barSupportsSlideBehavior(instance->barConfig)) {
    return true;
  }
  constexpr float kSettledThreshold = 0.999F;
  return instance->hideOpacity >= kSettledThreshold;
}

void Bar::revealAutoHideForAttachedPanel(wl_output* output, std::string_view barName) {
  BarInstance* instance = instanceForBar(output, barName);
  if (instance != nullptr) {
    revealAutoHideBar(*instance);
  }
}

void Bar::setAttachedPanelGeometry(
    wl_output* output, std::string_view barName, std::optional<AttachedPanelGeometry> geometry
) {
  BarInstance* instance = instanceForBar(output, barName);
  if (instance == nullptr) {
    return;
  }

  instance->attachedPanelGeometry = geometry;
  if (instance->surface != nullptr && instance->surface->width() > 0 && instance->surface->height() > 0) {
    applyBarShadowStyle(
        *instance, m_config->config().shell.shadow, static_cast<float>(instance->surface->width()),
        static_cast<float>(instance->surface->height())
    );
    instance->surface->requestRedraw();
  }
}

void Bar::beginAttachedPopup(wl_surface* surface) {
  auto* instance = instanceForSurface(surface);
  if (instance == nullptr) {
    return;
  }
  ++instance->attachedPopupCount;
}

void Bar::endAttachedPopup(wl_surface* surface) {
  auto* instance = instanceForSurface(surface);
  if (instance == nullptr) {
    return;
  }
  if (instance->attachedPopupCount > 0) {
    --instance->attachedPopupCount;
  }
  if (instance->attachedPopupCount > 0) {
    return;
  }
  instance->pointerInside =
      m_platform != nullptr && m_platform->hasPointerPosition() && m_platform->lastPointerSurface() == surface;
  if (instance->pointerInside) {
    instance->lastPointerSx = static_cast<float>(m_platform->lastPointerX());
    instance->lastPointerSy = static_cast<float>(m_platform->lastPointerY());
    instance->inputDispatcher.pointerEnter(
        instance->lastPointerSx, instance->lastPointerSy, m_platform->lastInputSerial()
    );
  } else {
    instance->inputDispatcher.pointerLeave();
  }
  if (instance->surface != nullptr) {
    instance->surface->requestRedraw();
  }
  if (!instance->pointerInside && m_hoveredInstance == instance) {
    m_hoveredInstance = nullptr;
  } else if (instance->pointerInside) {
    m_hoveredInstance = instance;
  }
  if (!barPointerHideAllowed(*instance) || instance->pointerInside) {
    return;
  }
  const bool suppressAutoHide =
      (m_autoHideSuppressionCallback != nullptr) ? m_autoHideSuppressionCallback(*instance) : false;
  if (!suppressAutoHide) {
    startHideFadeOut(*instance);
  }
}

void Bar::show() {
  for (const auto& instance : m_instances) {
    if (instance != nullptr) {
      instance->ipcLayoutReleased = false;
    }
  }
  applyIpcVisibility(true);
}

void Bar::hide() {
  for (const auto& instance : m_instances) {
    if (instance != nullptr && !barSupportsSlideBehavior(instance->barConfig)) {
      // bar-hide IPC always frees layout on non-autohide bars (v4 isVisible=false), regardless of reserve_space.
      instance->ipcLayoutReleased = true;
    }
  }
  applyIpcVisibility(false);
}

void Bar::suppressDisplay() {
  if (m_overlayDisplaySuppressed) {
    return;
  }
  m_overlayDisplaySuppressed = true;
  m_wasVisibleBeforeOverlaySuppress = isVisible();
  hide();
}

void Bar::unsuppressDisplay() {
  if (!m_overlayDisplaySuppressed) {
    return;
  }
  m_overlayDisplaySuppressed = false;
  if (m_wasVisibleBeforeOverlaySuppress) {
    show();
  }
}

void Bar::toggle() {
  const bool anyEffectivelyVisible = std::ranges::any_of(m_instances, [this](const auto& inst) {
    return inst != nullptr && instanceEffectivelyVisible(*inst);
  });

  if (anyEffectivelyVisible) {
    for (const auto& instance : m_instances) {
      if (instance != nullptr && !barSupportsSlideBehavior(instance->barConfig)) {
        instance->ipcLayoutReleased = true;
      }
    }
    applyIpcVisibility(false);
    return;
  }

  for (const auto& instance : m_instances) {
    if (instance != nullptr) {
      instance->ipcLayoutReleased = false;
    }
  }
  applyIpcVisibility(true);
}

