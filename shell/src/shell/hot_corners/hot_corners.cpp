#include "shell/hot_corners/hot_corners.h"

#include "app/application.h"
#include "compositors/compositor_detect.h"
#include "config/config_service.h"
#include "config/config_types.h"
#include "core/deferred_call.h"
#include "render/scene/input_area.h"
#include "ui/builders.h"
#include "wayland/settings_control.h"
#include "wayland/wayland_connection.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace {
  // Edge length (logical px) of each corner trigger surface. The cursor pins to the
  // exact corner pixel on a flick, so a tiny zone suffices; keep it minimal to
  // barely intercept pointer input over surfaces beneath it.
  constexpr std::int32_t kTriggerZoneSize = 2;

  void cornerAction(const HotCornersConfig& config, int position, std::string& action, std::string& command) {
    if (position == 0) {
      action = config.topLeft.action;
      command = config.topLeft.command;
    } else if (position == 1) {
      action = config.topRight.action;
      command = config.topRight.command;
    } else if (position == 2) {
      action = config.bottomLeft.action;
      command = config.bottomLeft.command;
    } else {
      action = config.bottomRight.action;
      command = config.bottomRight.command;
    }
  }
} // namespace

HotCorners::HotCorners(Application* app) : m_app(app) {}

HotCorners::~HotCorners() { destroySurfaces(); }

void HotCorners::initialize(WaylandConnection& wayland, ConfigService* config, RenderContext* renderContext) {
  m_wayland = &wayland;
  m_config = config;
  m_renderContext = renderContext;
  const auto [name, version] = wayland.desktopSettingsGlobal();
  if (compositors::isUmbriel() && name != 0 && config != nullptr && config->config().hotCorners.enabled) {
    m_migration = std::make_unique<SettingsControl>(wayland.registry(), name, version, [this]() {
      if (m_migration != nullptr && m_migration->ready() && !std::exchange(m_migrated, true)) {
        migrateToCompositor();
        DeferredCall::callLater([this]() { m_migration.reset(); });
      }
    });
  }
}

bool HotCorners::active() const {
  return m_config != nullptr && m_config->config().hotCorners.enabled && !compositors::isUmbriel();
}

void HotCorners::migrateToCompositor() {
  if (m_migration == nullptr || m_config == nullptr) {
    return;
  }
  SettingsControl& compositor = *m_migration;
  const HotCornersConfig& config = m_config->config().hotCorners;
  const std::array<std::pair<std::string_view, const HotCornersConfig::Corner*>, 4> corners{{
      {"top_left", &config.topLeft},
      {"top_right", &config.topRight},
      {"bottom_left", &config.bottomLeft},
      {"bottom_right", &config.bottomRight},
  }};
  const auto key = [](std::string_view corner, std::string_view field) {
    return std::format("hot_corners.{}.{}", corner, field);
  };
  const bool compositorHasCorners = std::ranges::any_of(corners, [&](const auto& entry) {
    return compositor.value(key(entry.first, "enabled")) == "true" || compositor.customized(key(entry.first, "action"));
  });
  if (!compositorHasCorners) {
    for (const auto& [corner, settings] : corners) {
      std::string action;
      if (settings->action == "launcher") {
        action = "shell:panel-toggle launcher";
      } else if (settings->action == "control_center") {
        action = "shell:panel-toggle control-center";
      } else if (settings->action == "window_switcher") {
        action = "shell:window-switcher";
      } else if (settings->action == "overview") {
        action = "overview-toggle";
      } else if (settings->action == "command" && !settings->command.empty()) {
        action = "spawn:" + settings->command;
      }
      if (action.empty()) {
        continue;
      }
      compositor.set(key(corner, "action"), action);
      compositor.set(key(corner, "delay_ms"), std::to_string(config.delayMs));
      compositor.set(key(corner, "enabled"), "true");
    }
  }
  (void)m_config->setOverride({"hot_corners", "enabled"}, ConfigOverrideValue{false});
}

void HotCorners::onConfigReload() {
  if (m_config == nullptr || m_wayland == nullptr) {
    return;
  }

  // Recreate whenever active (not just on a toggle): the resolved trigger layer follows the bar's layer, which a
  // reload may have changed.
  const bool enabled = active();
  if (enabled || enabled != m_lastEnabled) {
    onOutputChange();
  }
}

void HotCorners::onOutputChange() {
  if (m_config == nullptr || m_wayland == nullptr) {
    return;
  }
  m_lastEnabled = active();

  destroySurfaces();

  if (!m_lastEnabled) {
    return;
  }

  ensureSurfaces();
}

void HotCorners::ensureSurfaces() {
  for (const auto& out : m_wayland->outputs()) {
    if (!out.done || out.connectorName.empty()) {
      continue;
    }

    auto instance = std::make_unique<OutputInstance>();
    instance->output = out.output;

    const auto& config = m_config->config().hotCorners;

    if (config.topLeft.action != "none" && !config.topLeft.action.empty()) {
      buildCorner(instance->topLeft, 0, out.output);
    }
    if (config.topRight.action != "none" && !config.topRight.action.empty()) {
      buildCorner(instance->topRight, 1, out.output);
    }
    if (config.bottomLeft.action != "none" && !config.bottomLeft.action.empty()) {
      buildCorner(instance->bottomLeft, 2, out.output);
    }
    if (config.bottomRight.action != "none" && !config.bottomRight.action.empty()) {
      buildCorner(instance->bottomRight, 3, out.output);
    }

    m_instances.push_back(std::move(instance));
  }
}

void HotCorners::destroySurfaces() { m_instances.clear(); }

void HotCorners::triggerAction(const std::string& action, const std::string& command, wl_output* output) {
  if (action == "command") {
    if (!command.empty()) {
      m_app->runShellCommand(command);
    }
  } else if (action != "none" && !action.empty()) {
    m_app->triggerShellAction(action, output);
  }
}

void HotCorners::disarmCorner(Corner& corner) { corner.triggerTimer.stop(); }

void HotCorners::armCorner(Corner& corner, int position, wl_output* output) {
  if (m_config == nullptr) {
    return;
  }

  const auto& config = m_config->config().hotCorners;
  std::string action;
  std::string command;
  cornerAction(config, position, action, command);

  const std::int32_t delayMs = std::max(0, config.delayMs);
  if (delayMs <= 0) {
    corner.triggerTimer.stop();
    triggerAction(action, command, output);
    return;
  }

  corner.triggerTimer.start(
      std::chrono::milliseconds(delayMs), [this, action = std::move(action), command = std::move(command), output]() {
        triggerAction(action, command, output);
      }
  );
}

void HotCorners::buildCorner(Corner& corner, int position, wl_output* output) {
  // Sit on the highest layer any bar occupies on this output (Top or Overlay),
  // and create after the bar/dock so the trigger zone is never occluded by shell
  // chrome in the corner (a same-layer bar would otherwise swallow the pointer).
  // Tracking the bar's layer rather than always Overlay keeps the corners out of
  // the way of fullscreen/gaming surfaces when the bar is only on Top. Transient
  // surfaces opened later (panels, popups, the lock screen) still stack above.
  const LayerShellLayer layer = m_app->hotCornerLayerForOutput(output);
  constexpr std::int32_t size = kTriggerZoneSize;

  std::uint32_t anchor = 0;
  std::string cornerKey;
  if (position == 0) {
    anchor = LayerShellAnchor::Top | LayerShellAnchor::Left;
    cornerKey = "top_left";
  } else if (position == 1) {
    anchor = LayerShellAnchor::Top | LayerShellAnchor::Right;
    cornerKey = "top_right";
  } else if (position == 2) {
    anchor = LayerShellAnchor::Bottom | LayerShellAnchor::Left;
    cornerKey = "bottom_left";
  } else {
    anchor = LayerShellAnchor::Bottom | LayerShellAnchor::Right;
    cornerKey = "bottom_right";
  }

  LayerSurfaceConfig surfaceConfig{
      .nameSpace = "hot_corner_" + cornerKey,
      .layer = layer,
      .anchor = anchor,
      .width = static_cast<std::uint32_t>(size),
      .height = static_cast<std::uint32_t>(size),
      // -1: ignore other surfaces' exclusive zones so the corner anchors to the
      // absolute screen edge instead of being pushed inward by the bar's zone.
      .exclusiveZone = -1,
  };

  corner.surface = std::make_unique<LayerSurface>(*m_wayland, surfaceConfig);
  corner.surface->initialize(output);

  auto inputArea = ui::inputArea({});
  inputArea->setPosition(0, 0);
  inputArea->setSize(static_cast<float>(size), static_cast<float>(size));
  inputArea->setOnEnter([this, &corner, position, output](const InputArea::PointerData&) {
    armCorner(corner, position, output);
  });
  inputArea->setOnLeave([this, &corner]() { disarmCorner(corner); });

  corner.sceneRoot = std::move(inputArea);
  corner.inputDispatcher.setSceneRoot(corner.sceneRoot.get());

  corner.surface->setRenderContext(m_renderContext);
  corner.surface->setSceneRoot(corner.sceneRoot.get());
  corner.surface->requestRedraw();
}

bool HotCorners::onPointerEvent(const PointerEvent& event) {
  if (!m_lastEnabled || event.surface == nullptr) {
    return false;
  }

  for (const auto& instance : m_instances) {
    Corner* corners[] = {&instance->topLeft, &instance->topRight, &instance->bottomLeft, &instance->bottomRight};
    for (auto* corner : corners) {
      if (corner->surface && event.surface == corner->surface->wlSurface()) {
        switch (event.type) {
        case PointerEvent::Type::Enter:
          corner->inputDispatcher.pointerEnter(
              static_cast<float>(event.sx), static_cast<float>(event.sy), event.serial
          );
          return true;
        case PointerEvent::Type::Leave:
          corner->inputDispatcher.pointerLeave();
          return true;
        case PointerEvent::Type::Motion:
          corner->inputDispatcher.pointerMotion(
              static_cast<float>(event.sx), static_cast<float>(event.sy), event.serial
          );
          return true;
        case PointerEvent::Type::Button:
          return corner->inputDispatcher.pointerButton(
              static_cast<float>(event.sx), static_cast<float>(event.sy), event.button, event.pressed, event.serial,
              event.time, event.touch
          );
        case PointerEvent::Type::Axis:
          return corner->inputDispatcher.pointerAxis(
              static_cast<float>(event.sx), static_cast<float>(event.sy), event.axis, event.axisSource, event.axisValue,
              event.axisDiscrete, event.axisValue120, event.axisLines
          );
        }
        return false;
      }
    }
  }
  return false;
}
