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

std::optional<std::string> Bar::collectBarIpcInstances(
    std::optional<std::string> barName, std::optional<std::string> monitorSelector,
    std::vector<BarInstance*>& instancesOut
) {
  instancesOut.clear();

  if (m_config == nullptr) {
    return "error: config service not initialized\n";
  }

  if (barName.has_value()) {
    const bool knownBar = std::ranges::contains(m_config->config().bars, *barName, &BarConfig::name);
    if (!knownBar) {
      if (!monitorSelector.has_value()) {
        monitorSelector = std::move(barName);
        barName = std::nullopt;
      } else {
        std::vector<std::string> knownBars;
        knownBars.reserve(m_config->config().bars.size());
        for (const auto& bar : m_config->config().bars) {
          knownBars.push_back(bar.name);
        }
        const std::string suffix =
            knownBars.empty() ? std::string() : std::string("; known: ") + StringUtils::join(knownBars, ", ");
        return "error: unknown bar \"" + std::string(*barName) + "\"" + suffix + "\n";
      }
    }
  }

  const auto matchesBar = [&](const BarInstance& instance) {
    return !barName.has_value() || instance.barConfig.name == *barName;
  };

  if (!monitorSelector.has_value()) {
    for (const auto& instance : m_instances) {
      if (instance != nullptr && matchesBar(*instance)) {
        instancesOut.push_back(instance.get());
      }
    }
    if (instancesOut.empty()) {
      if (barName.has_value()) {
        return "error: no instances matched bar \"" + std::string(*barName) + "\"\n";
      }
      return "error: no bar instances are active\n";
    }
    return std::nullopt;
  }

  if (m_platform == nullptr) {
    return "error: bar service not initialized\n";
  }

  const std::string selector(*monitorSelector);
  std::vector<std::string> outputMatches;
  std::vector<std::string> knownOutputs;
  for (const auto& output : m_platform->outputs()) {
    if (output.connectorName.empty()) {
      continue;
    }
    knownOutputs.push_back(output.connectorName);
    if (outputMatchesSelector(selector, output)) {
      outputMatches.push_back(output.connectorName);
    }
  }

  std::ranges::sort(knownOutputs);
  knownOutputs.erase(std::ranges::unique(knownOutputs).begin(), knownOutputs.end());
  std::ranges::sort(outputMatches);
  outputMatches.erase(std::ranges::unique(outputMatches).begin(), outputMatches.end());

  if (outputMatches.empty()) {
    std::string error = "error: unknown monitor selector \"" + selector + "\"";
    if (!knownOutputs.empty()) {
      error += " (available: " + StringUtils::join(knownOutputs, ", ") + ")";
    }
    error += '\n';
    return error;
  }
  if (outputMatches.size() > 1) {
    return "error: monitor selector \""
        + selector
        + "\" matched multiple outputs: "
        + StringUtils::join(outputMatches, ", ")
        + "\n";
  }

  for (const auto& instance : m_instances) {
    if (instance == nullptr || instance->output == nullptr || !matchesBar(*instance)) {
      continue;
    }
    const auto it = std::find_if(
        m_platform->outputs().begin(), m_platform->outputs().end(),
        [&instance](const WaylandOutput& output) { return output.output == instance->output; }
    );
    if (it != m_platform->outputs().end() && it->connectorName == outputMatches.front()) {
      instancesOut.push_back(instance.get());
    }
  }

  if (instancesOut.empty()) {
    std::string error = "error: no instances matched";
    if (barName.has_value()) {
      error += " bar \"" + std::string(*barName) + "\"";
    }
    error += " on \"" + outputMatches.front() + "\"\n";
    return error;
  }

  return std::nullopt;
}

namespace {

  [[nodiscard]] std::optional<std::string> parseBarVisibilityIpcArgs(
      std::string_view command, std::string_view args, std::optional<std::string>& barName,
      std::optional<std::string>& monitorSelector
  ) {
    const auto parts = noctalia::ipc::splitWords(args);
    if (parts.size() > 2) {
      return "error: usage: " + std::string(command) + " [bar-name] [monitor-selector]\n";
    }
    barName = std::nullopt;
    monitorSelector = std::nullopt;
    if (!parts.empty() && !parts[0].empty()) {
      barName = parts[0];
    }
    if (parts.size() >= 2 && !parts[1].empty()) {
      monitorSelector = parts[1];
    }
    return std::nullopt;
  }

} // namespace

std::string Bar::showBarIpc(std::string_view args) {
  std::optional<std::string> barName;
  std::optional<std::string> monitorSelector;
  if (const auto parseError = parseBarVisibilityIpcArgs("bar-show", args, barName, monitorSelector)) {
    return *parseError;
  }

  std::vector<BarInstance*> targets;
  if (const auto collectError = collectBarIpcInstances(barName, monitorSelector, targets)) {
    return *collectError;
  }

  for (BarInstance* instance : targets) {
    instance->ipcLayoutReleased = false;
    setInstanceIpcVisible(*instance, true);
    syncBarSurfaceChrome(*instance);
  }
  return "ok\n";
}

std::string Bar::hideBarIpc(std::string_view args) {
  std::optional<std::string> barName;
  std::optional<std::string> monitorSelector;
  if (const auto parseError = parseBarVisibilityIpcArgs("bar-hide", args, barName, monitorSelector)) {
    return *parseError;
  }

  std::vector<BarInstance*> targets;
  if (const auto collectError = collectBarIpcInstances(barName, monitorSelector, targets)) {
    return *collectError;
  }

  for (BarInstance* instance : targets) {
    if (!barSupportsSlideBehavior(instance->barConfig)) {
      instance->ipcLayoutReleased = true;
    }
    setInstanceIpcVisible(*instance, false);
    syncBarSurfaceChrome(*instance);
  }
  return "ok\n";
}

std::string Bar::toggleBarIpc(std::string_view args) {
  std::optional<std::string> barName;
  std::optional<std::string> monitorSelector;
  if (const auto parseError = parseBarVisibilityIpcArgs("bar-toggle", args, barName, monitorSelector)) {
    return *parseError;
  }

  std::vector<BarInstance*> targets;
  if (const auto collectError = collectBarIpcInstances(barName, monitorSelector, targets)) {
    return *collectError;
  }

  const bool anyEffectivelyVisible = std::ranges::any_of(targets, [this](const BarInstance* instance) {
    return instance != nullptr && instanceEffectivelyVisible(*instance);
  });

  if (anyEffectivelyVisible) {
    for (BarInstance* instance : targets) {
      if (!barSupportsSlideBehavior(instance->barConfig)) {
        instance->ipcLayoutReleased = true;
      }
      setInstanceIpcVisible(*instance, false);
      syncBarSurfaceChrome(*instance);
    }
    return "ok\n";
  }

  for (BarInstance* instance : targets) {
    instance->ipcLayoutReleased = false;
    setInstanceIpcVisible(*instance, true);
    syncBarSurfaceChrome(*instance);
  }
  return "ok\n";
}

std::string Bar::toggleBarReserveSpaceIpc(std::string_view args) {
  std::optional<std::string> barName;
  std::optional<std::string> monitorSelector;
  if (const auto parseError = parseBarVisibilityIpcArgs("bar-reserve-toggle", args, barName, monitorSelector)) {
    return *parseError;
  }

  std::vector<BarInstance*> targets;
  if (const auto collectError = collectBarIpcInstances(barName, monitorSelector, targets)) {
    return *collectError;
  }

  for (BarInstance* instance : targets) {
    if (instance != nullptr) {
      instance->barConfig.reserveSpace = !instance->barConfig.reserveSpace;
      syncBarExclusiveZone(*instance);
    }
  }
  return "ok\n";
}

std::string Bar::setBarAutoHideIpc(std::string_view args) {
  if (m_config == nullptr) {
    return "error: config service not initialized\n";
  }

  const auto parts = noctalia::ipc::splitWords(args);
  if (parts.empty() || parts.size() > 3) {
    return "error: usage: bar-auto-hide-set <on|off|smart|true|false|1|0> [bar-name] [monitor-selector]\n";
  }

  const std::string& value = parts[0];
  bool enabled = false;
  bool smart = false;
  if (value == "on" || value == "true" || value == "1") {
    enabled = true;
  } else if (value == "off" || value == "false" || value == "0") {
    enabled = false;
  } else if (value == "smart") {
    smart = true;
  } else {
    return "error: invalid value (use on/off/smart, true/false, 1/0)\n";
  }

  std::optional<std::string> barName;
  std::optional<std::string> monitorSelector;
  if (parts.size() >= 2 && !parts[1].empty()) {
    barName = parts[1];
  }
  if (parts.size() >= 3 && !parts[2].empty()) {
    monitorSelector = parts[2];
  }

  std::vector<BarInstance*> targets;
  if (const auto collectError = collectBarIpcInstances(barName, monitorSelector, targets)) {
    return *collectError;
  }

  auto applyTransientAutoHide = [this, enabled, smart](BarInstance& instance) {
    auto applySurfaceSpec = [this](BarInstance& inst) {
      if (inst.surface == nullptr) {
        return;
      }
      const auto spec = computeBarSurfaceSpec(inst.barConfig, m_config->config().shell.shadow);
      inst.surface->setMargins(spec.marginTop, spec.marginRight, spec.marginBottom, spec.marginLeft);
      inst.surface->requestSize(spec.surfaceWidth, spec.surfaceHeight);
    };

    instance.ipcLayoutReleased = false;
    instance.autoHideDisablePending = false;
    instance.autoHideDelayTimer.stop();
    instance.animations.cancelForOwner(instance.slideRoot);

    instance.barConfig.autoHide = enabled;
    instance.barConfig.smartAutoHide = smart;

    if (enabled || smart) {
      applySurfaceSpec(instance);
      if (instance.slideRoot != nullptr) {
        instance.slideRoot->setOpacity(1.0F);
      }
      const bool suppressAutoHide =
          (m_autoHideSuppressionCallback != nullptr) ? m_autoHideSuppressionCallback(instance) : false;
      if (instance.pointerInside || instance.attachedPopupCount > 0 || suppressAutoHide) {
        revealAutoHideBar(instance);
      } else {
        startHideFadeOut(instance, true);
      }
      return;
    }

    if (instance.barConfig.autoHide && instance.hideOpacity < 0.999F) {
      const float current = instance.hideOpacity;
      instance.autoHideDisablePending = true;
      const auto settled = [this, inst = &instance, applySurfaceSpec] {
        inst->autoHideDisablePending = false;
        inst->barConfig.autoHide = false;
        applySurfaceSpec(*inst);
        syncBarSlideLayerTransform(*inst);
        syncBarAutoHideInputRegion(*inst);
        syncBarSurfaceChrome(*inst);
        if (inst->surface != nullptr) {
          inst->surface->requestRedraw();
        }
      };
      if (!slideOnCompositor(instance, true, settled)) {
        instance.slideAnimId = instance.animations.drive(
            instance.slideAnimId, current, 1.0F, Style::spring(Tokens::springMove),
            [this, inst = &instance](float v) {
              inst->hideOpacity = v;
              syncBarSlideLayerTransform(*inst);
              syncBarSurfaceChrome(*inst);
            },
            settled, instance.slideRoot, 0.0F, Threshold::px(static_cast<float>(instance.barConfig.thickness))
        );
      }
      if (instance.surface != nullptr) {
        instance.surface->requestRedraw();
      }
      return;
    }

    instance.barConfig.autoHide = false;
    instance.autoHideDisablePending = false;
    instance.hideOpacity = 1.0F;
    if (instance.slideRoot != nullptr) {
      instance.slideRoot->setOpacity(1.0F);
    }
    applySurfaceSpec(instance);
    syncBarSlideLayerTransform(instance);
    syncBarAutoHideInputRegion(instance);
    syncBarSurfaceChrome(instance);
    if (instance.surface != nullptr) {
      instance.surface->requestRedraw();
    }
  };

  for (BarInstance* instance : targets) {
    applyTransientAutoHide(*instance);
  }
  return "ok\n";
}

std::string Bar::setBarLayerIpc(std::string_view args) {
  const auto parts = noctalia::ipc::splitWords(args);
  if (parts.empty() || parts.size() > 3) {
    return "error: usage: bar-layer-set <top|overlay> [bar-name] [monitor-selector]\n";
  }

  const std::string& layer = parts[0];
  if (layer != "top" && layer != "overlay") {
    return "error: invalid layer (use top or overlay)\n";
  }

  std::optional<std::string> barName;
  std::optional<std::string> monitorSelector;
  if (parts.size() >= 2 && !parts[1].empty()) {
    barName = parts[1];
  }
  if (parts.size() >= 3 && !parts[2].empty()) {
    monitorSelector = parts[2];
  }

  std::vector<BarInstance*> targets;
  if (const auto collectError = collectBarIpcInstances(barName, monitorSelector, targets)) {
    return *collectError;
  }

  const LayerShellLayer shellLayer = layerShellLayerFromConfig(layer);
  for (BarInstance* instance : targets) {
    if (instance == nullptr || instance->surface == nullptr) {
      continue;
    }
    instance->surface->setLayer(shellLayer);
    instance->barConfig.layer = layer;
  }

  return "ok\n";
}

TaskbarWidget* Bar::findTaskbarWidget(const IpcInvocationContext& context) const {
  const auto findIn = [&context](const std::vector<std::unique_ptr<Widget>>& widgets) -> TaskbarWidget* {
    for (const auto& widget : widgets) {
      if (widget->configName() != context.widgetName) {
        continue;
      }
      if (auto* taskbar = dynamic_cast<TaskbarWidget*>(widget.get()); taskbar != nullptr) {
        return taskbar;
      }
    }
    return nullptr;
  };

  for (const auto& instance : m_instances) {
    if (instance->barConfig.name != context.barName || instance->output != context.output) {
      continue;
    }
    for (const auto* section : {&instance->startWidgets, &instance->centerWidgets, &instance->endWidgets}) {
      if (auto* taskbar = findIn(*section); taskbar != nullptr) {
        return taskbar;
      }
    }
  }
  return nullptr;
}

void Bar::registerIpc(IpcService& ipc) {
  // Widget gesture actions dispatch through the same registry.
  m_actionDispatcher.setIpcService(&ipc);

  ipc.bindCycle(noctalia::cli::msg::taskbarCycle, [this, &ipc](const std::string& args) -> std::string {
    const auto parts = noctalia::ipc::splitWords(args);
    if (parts.size() != 1 || (parts[0] != "next" && parts[0] != "prev")) {
      return "error: taskbar-cycle requires <next|prev>\n";
    }
    // Order comes from the taskbar's own model (pins, grouping, per-monitor filter), so the
    // target is a widget instance rather than a global window list.
    const auto& context = ipc.invocationContext();
    if (!context.has_value() || context->widgetName.empty()) {
      return "error: taskbar-cycle must be invoked from a taskbar widget gesture\n";
    }
    auto* taskbar = findTaskbarWidget(*context);
    if (taskbar == nullptr) {
      return "error: no taskbar widget named '" + context->widgetName + "' on bar '" + context->barName + "'\n";
    }
    taskbar->cycleAdjacent(parts[0] == "next" ? 1 : -1);
    return "ok\n";
  });

  ipc.bind(noctalia::cli::msg::barShow, [this](const std::string& args) -> std::string { return showBarIpc(args); });

  ipc.bind(noctalia::cli::msg::barHide, [this](const std::string& args) -> std::string { return hideBarIpc(args); });

  ipc.bind(noctalia::cli::msg::barToggle, [this](const std::string& args) -> std::string {
    return toggleBarIpc(args);
  });

  ipc.bind(noctalia::cli::msg::barReserveToggle, [this](const std::string& args) -> std::string {
    return toggleBarReserveSpaceIpc(args);
  });

  ipc.bind(noctalia::cli::msg::barAutoHideSet, [this](const std::string& args) -> std::string {
    return setBarAutoHideIpc(args);
  });

  ipc.bind(noctalia::cli::msg::barLayerSet, [this](const std::string& args) -> std::string {
    return setBarLayerIpc(args);
  });
}
