#include "shell/lockscreen/lockscreen_widgets_host.h"

#include "config/config_service.h"
#include "render/render_context.h"
#include "render/scene/node.h"
#include "render/scene/rect_node.h"
#include "core/log.h"
#include "scripting/pixel_stream_view.h"
#include "scripting/plugin_id.h"
#include "scripting/plugin_registry.h"
#include "scripting/script_api_context.h"
#include "shell/desktop/desktop_widget_layout.h"
#include "shell/lockscreen/lock_screen.h"
#include "shell/lockscreen/lock_surface.h"
#include "shell/lockscreen/lockscreen_login_box.h"
#include "time/time_format.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "wayland/wayland_connection.h"

#include <algorithm>
#include <string>
#include <wayland-client-protocol.h>

namespace {

  constexpr Logger kLog("lock-widgets");
  // The helper starts after the first lock frame is out; slots show the placeholder until then.
  constexpr std::chrono::milliseconds kHelperStartDelay{250};
  constexpr int kMaxHelperRestarts = 3;
  // Size of a plugin slot whose box is auto (0): the helper needs a fixed frame size.
  constexpr float kDefaultStreamSize = 160.0F;
  constexpr float kPlaceholderRadius = 16.0F;

  DesktopWidgetState* findStateById(LockscreenWidgetsSnapshot& snapshot, const std::string& id) {
    for (auto& widget : snapshot.widgets) {
      if (widget.id == id) {
        return &widget;
      }
    }
    return nullptr;
  }

} // namespace

void LockscreenWidgetsHost::initialize(const DesktopWidgetServices& services) {
  m_wayland = &services.wayland;
  m_config = services.config;
  m_renderContext = services.renderContext;
  m_factory = std::make_unique<DesktopWidgetFactory>(services.runtime);
  m_scriptApi = services.runtime.scriptDeps.scriptApi;
}

void LockscreenWidgetsHost::show(const LockscreenWidgetsSnapshot& snapshot, LockScreen& lockScreen) {
  m_snapshot = snapshot;
  m_visible = true;
  m_helperRestarts = 0;
  syncSurfaces(lockScreen);
}

void LockscreenWidgetsHost::hide() {
  m_helperTimer.stop();
  m_helper.stop();
  m_helperRestarts = 0;
  for (auto& instance : m_instances) {
    if (instance->widget != nullptr) {
      instance->widget->setFrameTickRequestCallback(nullptr);
      instance->widget->setUpdateCallback(nullptr);
      instance->widget->setLayoutCallback(nullptr);
      instance->widget->setRedrawCallback(nullptr);
    }
    detachFromSurface(*instance);
  }
  m_visible = false;
  m_instances.clear();
}

void LockscreenWidgetsHost::rebuild(const LockscreenWidgetsSnapshot& snapshot, LockScreen& lockScreen) {
  m_snapshot = snapshot;
  if (!m_visible) {
    return;
  }
  syncSurfaces(lockScreen);
}

void LockscreenWidgetsHost::reloadPluginWidgets(LockScreen& lockScreen) {
  if (!m_visible) {
    return;
  }
  // Match on the id syntax, not the registry: a plugin disabled by this reload has
  // already left the registry, and its instances must go down with it.
  const auto before = m_instances.size();
  std::erase_if(m_instances, [this](std::unique_ptr<WidgetInstance>& instance) {
    if (!scripting::isValidPluginEntryId(instance->state.type)) {
      return false;
    }
    detachFromSurface(*instance);
    return true;
  });
  if (m_instances.size() == before) {
    return;
  }
  syncSurfaces(lockScreen);
}

void LockscreenWidgetsHost::onOutputChange(LockScreen& lockScreen) {
  if (!m_visible) {
    return;
  }
  syncSurfaces(lockScreen);
}

void LockscreenWidgetsHost::onSecondTick() {
  if (!m_visible) {
    return;
  }

  const bool minuteBoundary = formatLocalTime("{:%S}") == "00";
  for (auto& instance : m_instances) {
    if (instance->surface == nullptr || instance->widget == nullptr) {
      continue;
    }
    if (instance->widget->wantsSecondTicks()) {
      instance->surface->requestUpdateOnly();
    } else if (minuteBoundary) {
      instance->surface->requestUpdate();
    }
  }
}

bool LockscreenWidgetsHost::wantsSecondTicks() const {
  if (!m_visible) {
    return false;
  }
  for (const auto& instance : m_instances) {
    if (instance->widget != nullptr && instance->widget->wantsSecondTicks()) {
      return true;
    }
  }
  return false;
}

LockscreenWidgetsHost::WidgetInstance* LockscreenWidgetsHost::findInstance(const std::string& id) {
  for (auto& instance : m_instances) {
    if (instance->state.id == id) {
      return instance.get();
    }
  }
  return nullptr;
}

LockSurface* LockscreenWidgetsHost::findSurfaceForOutput(LockScreen& lockScreen, const std::string& outputKey) const {
  LockSurface* found = nullptr;
  lockScreen.forEachSurface([&](LockSurface& surface) {
    if (found != nullptr || m_wayland == nullptr) {
      return;
    }
    const WaylandOutput* output = m_wayland->findOutputByWl(surface.output());
    if (output == nullptr) {
      return;
    }
    if (desktop_widgets::outputKey(*output) == outputKey) {
      found = &surface;
    }
  });
  return found;
}

void LockscreenWidgetsHost::syncSurfaces(LockScreen& lockScreen) {
  if (!m_visible || m_wayland == nullptr || m_renderContext == nullptr || m_factory == nullptr) {
    return;
  }

  std::erase_if(m_instances, [this](std::unique_ptr<WidgetInstance>& instance) {
    if (instance->wallpaper) {
      return false;
    }
    const DesktopWidgetState* state = findStateById(m_snapshot, instance->state.id);
    if (state == nullptr || !state->enabled) {
      detachFromSurface(*instance);
      return true;
    }
    return instance->surface == nullptr;
  });

  for (const auto& state : m_snapshot.widgets) {
    if (!state.enabled || lockscreen_login_box::isLoginBoxWidget(state)) {
      continue;
    }

    const WaylandOutput* output = desktop_widgets::resolveStateOutput(*m_wayland, state);
    if (output == nullptr) {
      std::erase_if(m_instances, [this, &state](std::unique_ptr<WidgetInstance>& instance) {
        if (instance->state.id == state.id) {
          detachFromSurface(*instance);
          return true;
        }
        return false;
      });
      continue;
    }

    const std::string outputKey = desktop_widgets::outputKey(*output);
    LockSurface* surface = findSurfaceForOutput(lockScreen, outputKey);
    if (surface == nullptr) {
      std::erase_if(m_instances, [this, &state](std::unique_ptr<WidgetInstance>& instance) {
        if (instance->state.id == state.id) {
          detachFromSurface(*instance);
          return true;
        }
        return false;
      });
      continue;
    }

    WidgetInstance* existing = findInstance(state.id);
    if (existing == nullptr) {
      createInstance(state, *surface, *output);
      continue;
    }

    // A plugin slot's frame size is its box, so a resize needs a new helper slot.
    const bool widgetDefinitionChanged = existing->state.type != state.type
        || existing->state.settings != state.settings
        || existing->surface != surface
        || (existing->stream != nullptr
            && (existing->state.boxWidth != state.boxWidth || existing->state.boxHeight != state.boxHeight));

    if (widgetDefinitionChanged) {
      std::erase_if(m_instances, [this, &state](std::unique_ptr<WidgetInstance>& instance) {
        if (instance->state.id != state.id) {
          return false;
        }
        detachFromSurface(*instance);
        return true;
      });
      createInstance(state, *surface, *output);
      continue;
    }

    if (!(existing->state == state)) {
      existing->state = state;
      if (existing->surface != nullptr) {
        existing->surface->requestLayout();
      }
    }
  }

  syncWallpapers(lockScreen);
  lockScreen.forEachSurface([&](LockSurface& surface) { surface.setWidgetsHost(this); });
}

void LockscreenWidgetsHost::syncWallpapers(LockScreen& lockScreen) {
  std::vector<DesktopWidgetState> wanted;
  std::vector<LockSurface*> surfaces;
  lockScreen.forEachSurface([&](LockSurface& surface) {
    const WaylandOutput* output = m_wayland->findOutputByWl(surface.output());
    if (output == nullptr || surface.isBlackout()) {
      return;
    }
    const std::string path = lockScreen.wallpaperPathForOutput(output->connectorName);
    DesktopWidgetState state;
    state.type = LockScreen::liveWallpaperType(path);
    if (state.type.empty()) {
      return;
    }
    state.id = "@wallpaper:" + output->connectorName;
    state.settings.emplace("path", path);
    wanted.push_back(std::move(state));
    surfaces.push_back(&surface);
  });
  std::erase_if(m_instances, [&](std::unique_ptr<WidgetInstance>& instance) {
    if (!instance->wallpaper) {
      return false;
    }
    for (std::size_t i = 0; i < wanted.size(); ++i) {
      if (wanted[i].id == instance->state.id && wanted[i].type == instance->state.type
          && wanted[i].settings == instance->state.settings && surfaces[i] == instance->surface) {
        return false;
      }
    }
    detachFromSurface(*instance);
    return true;
  });
  for (std::size_t i = 0; i < wanted.size(); ++i) {
    if (findInstance(wanted[i].id) == nullptr) {
      createStreamInstance(wanted[i], *surfaces[i], true);
    }
  }
}

void LockscreenWidgetsHost::createInstance(
    const DesktopWidgetState& state, LockSurface& surface, const WaylandOutput& output
) {
  if (m_factory == nullptr || m_renderContext == nullptr) {
    return;
  }
  // Plugin code, user shaders and video decode run only in the lock helper.
  if (scripting::isValidPluginEntryId(state.type) || state.type == "shader" || state.type == "video") {
    createStreamInstance(state, surface);
    return;
  }

  const float baseUiScale = m_config != nullptr ? m_config->config().accessibility.uiScale : 1.0F;
  auto widget = m_factory->create(state.type, state.settings, desktop_widgets::widgetContentScale(baseUiScale));
  if (widget == nullptr) {
    return;
  }

  widget->create();
  widget->setBox(state.boxWidth, state.boxHeight);

  if (surface.renderTarget().isReady()) {
    m_renderContext->makeCurrent(surface.renderTarget());
    Renderer& renderer = surface.renderTarget().renderer();
    widget->update(renderer);
    widget->layout(renderer);
  }

  const float intrinsicWidth = std::max(1.0F, widget->intrinsicWidth());
  const float intrinsicHeight = std::max(1.0F, widget->intrinsicHeight());

  DesktopWidgetState clampedState = state;
  desktop_widgets::clampStateToOutput(*m_wayland, clampedState, intrinsicWidth, intrinsicHeight);

  auto instance = std::make_unique<WidgetInstance>();
  instance->state = std::move(clampedState);
  instance->surface = &surface;
  instance->widget = std::move(widget);
  instance->intrinsicWidth = intrinsicWidth;
  instance->intrinsicHeight = intrinsicHeight;

  auto* rawInstance = instance.get();
  instance->widget->setAnimationManager(&instance->animations);
  instance->widget->setUpdateCallback([rawInstance] {
    if (rawInstance->surface != nullptr) {
      rawInstance->surface->requestUpdateOnly();
    }
  });
  instance->widget->setLayoutCallback([rawInstance] {
    if (rawInstance->surface != nullptr) {
      rawInstance->surface->requestUpdate();
    }
  });
  instance->widget->setRedrawCallback([rawInstance] {
    if (rawInstance->surface != nullptr) {
      rawInstance->surface->requestRedraw();
    }
  });
  instance->widget->setFrameTickRequestCallback([this, rawInstance] {
    if (rawInstance->surface != nullptr) {
      syncSurfaceFrameTick(rawInstance->surface);
      rawInstance->surface->requestFrameTick();
    }
  });

  attachToSurface(*instance);
  m_instances.push_back(std::move(instance));
  (void)output;
}

void LockscreenWidgetsHost::attachToSurface(WidgetInstance& instance) {
  if (instance.surface == nullptr || instance.widget == nullptr) {
    return;
  }

  Node* layer = instance.surface->widgetLayer();
  if (layer == nullptr) {
    return;
  }

  auto transformNode = ui::node({});
  transformNode->setAnimationManager(&instance.animations);
  instance.transformNode = layer->addChild(std::move(transformNode));
  instance.transformNode->addChild(instance.widget->releaseRoot());

  syncSurfaceFrameTick(instance.surface);
  instance.surface->requestLayout();
}

void LockscreenWidgetsHost::syncSurfaceFrameTick(LockSurface* surfacePtr) {
  if (surfacePtr == nullptr) {
    return;
  }

  const bool hasWidgets = std::ranges::any_of(m_instances, [&](const auto& instance) {
    return instance->surface == surfacePtr && instance->widget != nullptr;
  });
  if (!hasWidgets) {
    surfacePtr->setFrameTickCallback(nullptr);
    return;
  }

  auto* host = this;
  surfacePtr->setFrameTickCallback([host, surfacePtr](float deltaMs) {
    if (host->m_renderContext == nullptr) {
      return;
    }
    host->m_renderContext->makeCurrent(surfacePtr->renderTarget());
    Renderer& renderer = surfacePtr->renderTarget().renderer();

    bool needsRedraw = false;
    for (auto& instance : host->m_instances) {
      if (instance->surface != surfacePtr) {
        continue;
      }
      instance->animations.tick(deltaMs);
      needsRedraw = needsRedraw || instance->animations.hasActive();
    }

    bool needsContinuousRedraw = needsRedraw;
    for (auto& instance : host->m_instances) {
      if (instance->surface != surfacePtr || instance->widget == nullptr || !instance->widget->needsFrameTick()) {
        continue;
      }
      instance->widget->onFrameTick(deltaMs, renderer);
      needsContinuousRedraw = true;
    }

    if (needsContinuousRedraw) {
      surfacePtr->requestRedraw();
    }
  });
}

void LockscreenWidgetsHost::detachFromSurface(WidgetInstance& instance) {
  LockSurface* surface = instance.surface;

  if (instance.transformNode != nullptr && surface != nullptr) {
    if (Node* layer = instance.transformNode->parent(); layer != nullptr) {
      (void)layer->removeChild(instance.transformNode);
    }
    instance.transformNode = nullptr;
  }
  instance.surface = nullptr;

  if (surface != nullptr) {
    syncSurfaceFrameTick(surface);
  }
}

void LockscreenWidgetsHost::prepareFrame(LockSurface& surface, bool needsUpdate, bool needsLayout) {
  if (!m_visible || m_renderContext == nullptr) {
    return;
  }

  m_renderContext->makeCurrent(surface.renderTarget());
  Renderer& renderer = surface.renderTarget().renderer();

  const float baseUiScale = m_config != nullptr ? m_config->config().accessibility.uiScale : 1.0F;
  const auto surfaceW = static_cast<float>(surface.width());
  const auto surfaceH = static_cast<float>(surface.height());

  Node* layer = surface.widgetLayer();
  if (layer != nullptr) {
    layer->setPosition(0.0F, 0.0F);
    layer->setSize(surfaceW, surfaceH);
  }
  if (Node* live = surface.liveWallpaperLayer(); live != nullptr) {
    live->setPosition(0.0F, 0.0F);
    live->setSize(surfaceW, surfaceH);
  }

  for (auto& instance : m_instances) {
    if (instance->surface != &surface || (instance->widget == nullptr && instance->stream == nullptr)) {
      continue;
    }
    if (instance->wallpaper) {
      // The helper renders at the surface size: a new size needs a new helper slot.
      if ((instance->intrinsicWidth != surfaceW || instance->intrinsicHeight != surfaceH) && surfaceW > 0.0F
          && surfaceH > 0.0F) {
        instance->intrinsicWidth = surfaceW;
        instance->intrinsicHeight = surfaceH;
        instance->stream->setSize(surfaceW, surfaceH);
        if (!m_helperTimer.active()) {
          scheduleHelper(kHelperStartDelay);
        }
      }
      instance->state.cx = surfaceW * 0.5F;
      instance->state.cy = surfaceH * 0.5F;
    } else if (instance->widget == nullptr && m_wayland != nullptr) {
      desktop_widgets::clampStateToOutput(
          *m_wayland, instance->state, instance->intrinsicWidth, instance->intrinsicHeight
      );
    }
    if (instance->widget == nullptr) {
      if (instance->streamScale > 0.0F && instance->streamScale != surface.renderTarget().contentScale()
          && !m_helperTimer.active()) {
        scheduleHelper(kHelperStartDelay);
      }
      instance->transformNode->setFrameSize(instance->intrinsicWidth, instance->intrinsicHeight);
      instance->transformNode->setPosition(
          instance->state.cx - instance->intrinsicWidth * 0.5F, instance->state.cy - instance->intrinsicHeight * 0.5F
      );
      instance->transformNode->setRotation(instance->state.rotationRad);
      float flipX = 1.0F;
      float flipY = 1.0F;
      desktop_widgets::widgetNodeScale(instance->state, flipX, flipY);
      instance->transformNode->setScale(flipX, flipY);
      // No DesktopWidget lays this subtree out, and the stream view uploads its frames in layout.
      instance->transformNode->layout(renderer);
      continue;
    }

    instance->widget->setContentScale(desktop_widgets::widgetContentScale(baseUiScale));
    instance->widget->setBox(instance->state.boxWidth, instance->state.boxHeight);

    if (needsUpdate) {
      instance->widget->update(renderer);
    }
    if (needsLayout) {
      instance->widget->layout(renderer);
      instance->intrinsicWidth = std::max(1.0F, instance->widget->intrinsicWidth());
      instance->intrinsicHeight = std::max(1.0F, instance->widget->intrinsicHeight());
    }

    if (m_wayland != nullptr) {
      DesktopWidgetState currentState = instance->state;
      if (const DesktopWidgetState* origState = findStateById(m_snapshot, instance->state.id); origState != nullptr) {
        currentState = *origState;
      }
      desktop_widgets::clampStateToOutput(
          *m_wayland, currentState, instance->intrinsicWidth, instance->intrinsicHeight
      );
      instance->state = currentState;
    }

    if (instance->transformNode == nullptr) {
      continue;
    }

    instance->transformNode->setFrameSize(instance->intrinsicWidth, instance->intrinsicHeight);
    instance->transformNode->setPosition(
        instance->state.cx - instance->intrinsicWidth * 0.5F, instance->state.cy - instance->intrinsicHeight * 0.5F
    );
    instance->transformNode->setRotation(instance->state.rotationRad);
    float flipScaleX = 1.0F;
    float flipScaleY = 1.0F;
    desktop_widgets::widgetNodeScale(instance->state, flipScaleX, flipScaleY);
    instance->transformNode->setScale(flipScaleX, flipScaleY);
  }

  // Mirror the desktop host: widgets like sysmon drive updates from frame ticks.
  const bool needsFrameTick = std::ranges::any_of(m_instances, [&surface](const auto& instance) {
    return instance != nullptr
        && instance->surface == &surface
        && instance->widget != nullptr
        && instance->widget->needsFrameTick();
  });
  if (needsFrameTick) {
    surface.requestFrameTick();
  }
}

void LockscreenWidgetsHost::createStreamInstance(const DesktopWidgetState& state, LockSurface& surface, bool wallpaper) {
  auto instance = std::make_unique<WidgetInstance>();
  instance->state = state;
  instance->surface = &surface;
  instance->wallpaper = wallpaper;
  if (wallpaper) {
    instance->intrinsicWidth = std::max(1.0F, static_cast<float>(surface.width()));
    instance->intrinsicHeight = std::max(1.0F, static_cast<float>(surface.height()));
  } else {
    instance->intrinsicWidth = state.boxWidth > 0.0F ? state.boxWidth : kDefaultStreamSize;
    instance->intrinsicHeight = state.boxHeight > 0.0F ? state.boxHeight : kDefaultStreamSize;
  }

  auto transformNode = ui::node({});
  transformNode->setAnimationManager(&instance->animations);
  if (!wallpaper) {
    auto placeholder = std::make_unique<RectNode>();
    placeholder->setSize(instance->intrinsicWidth, instance->intrinsicHeight);
    placeholder->setStyle({.fill = colorForRole(ColorRole::Fill1), .radius = kPlaceholderRadius});
    instance->placeholder = transformNode->addChild(std::move(placeholder));
  }
  auto stream = std::make_unique<PixelStreamView>();
  stream->setSize(instance->intrinsicWidth, instance->intrinsicHeight);
  instance->stream = static_cast<PixelStreamView*>(transformNode->addChild(std::move(stream)));

  if (Node* layer = wallpaper ? surface.liveWallpaperLayer() : surface.widgetLayer(); layer != nullptr) {
    instance->transformNode = layer->addChild(std::move(transformNode));
  }
  m_instances.push_back(std::move(instance));
  surface.requestLayout();
  scheduleHelper(kHelperStartDelay);
}

void LockscreenWidgetsHost::scheduleHelper(std::chrono::milliseconds delay) {
  m_helperTimer.start(delay, [this] { startHelper(); });
}

void LockscreenWidgetsHost::startHelper() {
  if (!m_visible) {
    return;
  }
  std::vector<WidgetInstance*> streams;
  std::vector<LockHelperProcess::Slot> slots;
  LockHelperProcess::StartData data;
  const float baseUiScale = m_config != nullptr ? m_config->config().accessibility.uiScale : 1.0F;
  auto& registry = scripting::PluginRegistry::instance();
  for (auto& instance : m_instances) {
    if (instance->stream == nullptr || instance->surface == nullptr) {
      continue;
    }
    const std::string& type = instance->state.type;
    const std::string pluginId = type.substr(0, type.find(':'));
    if (const auto dir = registry.findPluginDir(pluginId); dir.has_value()) {
      const std::string source = dir->parent_path().string();
      if (std::ranges::find(data.pluginSources, source) == data.pluginSources.end()) {
        data.pluginSources.push_back(source);
      }
    }
    if (m_config != nullptr) {
      const auto& all = m_config->config().plugins.pluginSettings;
      if (const auto it = all.find(pluginId); it != all.end()) {
        data.pluginSettings.emplace(pluginId, it->second);
      }
    }
    slots.push_back({
        .type = type,
        .settings = instance->state.settings,
        .width = instance->intrinsicWidth,
        .height = instance->intrinsicHeight,
        .scale = instance->streamScale = instance->surface->renderTarget().contentScale(),
        .contentScale = desktop_widgets::widgetContentScale(baseUiScale),
    });
    streams.push_back(instance.get());
  }
  if (m_scriptApi != nullptr) {
    data.dark = m_scriptApi->isDarkMode();
    data.timeFormat = m_scriptApi->timeFormat();
    data.dateFormat = m_scriptApi->dateFormat();
  }

  const std::vector<int> fds = m_helper.start(slots, data);
  if (fds.empty() && !streams.empty()) {
    onStreamDropped();
  }
  for (std::size_t i = 0; i < streams.size(); ++i) {
    WidgetInstance* instance = streams[i];
    Node* placeholder = instance->placeholder;
    if (placeholder != nullptr) {
      placeholder->setVisible(true);
    }
    if (i >= fds.size()) {
      continue;
    }
    instance->stream->adopt(fds[i], [this, placeholder](bool connected) {
      if (placeholder != nullptr) {
        placeholder->setVisible(!connected);
      }
      if (!connected) {
        onStreamDropped();
      }
    });
  }
  applyStreamPause();
}

void LockscreenWidgetsHost::setStreamsPaused(bool powerSaver, std::unordered_map<std::string, bool> outputPower) {
  m_powerSaver = powerSaver;
  m_outputPower = std::move(outputPower);
  applyStreamPause();
}

void LockscreenWidgetsHost::applyStreamPause() {
  for (const auto& instance : m_instances) {
    if (instance->stream == nullptr || instance->surface == nullptr || m_wayland == nullptr) {
      continue;
    }
    const WaylandOutput* output = m_wayland->findOutputByWl(instance->surface->output());
    const auto power = output != nullptr ? m_outputPower.find(output->connectorName) : m_outputPower.end();
    // Power saver stops only the slots that draw every frame; plugin widgets keep updating.
    const bool animated = instance->state.type == "video" || instance->state.type == "shader";
    instance->stream->setPaused((m_powerSaver && animated) || (power != m_outputPower.end() && !power->second));
  }
}

void LockscreenWidgetsHost::onStreamDropped() {
  // Runs inside the stream's dispatch: restart from a timer, never here.
  if (m_helperTimer.active()) {
    return;
  }
  if (m_helperRestarts >= kMaxHelperRestarts) {
    kLog.warn("lock helper stopped {} times; plugin widgets stay blank until the next lock", m_helperRestarts);
    return;
  }
  ++m_helperRestarts;
  scheduleHelper(kHelperStartDelay * (1 << m_helperRestarts));
}
