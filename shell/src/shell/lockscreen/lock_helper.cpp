#include "shell/lockscreen/lock_helper.h"

#include "config/widget_config.h"
#include "core/deferred_call.h"
#include "core/log.h"
#include "core/timer_manager.h"
#include "core/toml.h"
#include "ipc/pixel_stream_server.h"
#include "render/animation/animation_manager.h"
#include "render/backend/render_backend.h"
#include "render/gl_shared_context.h"
#include "render/render_context.h"
#include "render/render_target.h"
#include "render/scene/node.h"
#include "scripting/plugin_registry.h"
#include "scripting/script_api_context.h"
#include "shell/desktop/desktop_widget_factory.h"
#include "shell/lockscreen/lock_shader.h"
#include "util/file_utils.h"
#include "ui/builders.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <poll.h>
#include <string>
#include <unistd.h>
#include <unordered_map>
#include <vector>

namespace {

  constexpr Logger kLog("lock-helper");
  constexpr int kFrameIntervalMs = 16;

  using SettingsMap = std::unordered_map<std::string, WidgetSettingValue>;

  struct Slot {
    explicit Slot(int fd) : server(fd) {}

    PixelStreamServer server;
    RenderTarget target;
    // Before root: node destructors cancel their animations here.
    AnimationManager animations;
    Node root;
    std::unique_ptr<DesktopWidget> widget;
    // Set instead of widget for a `shader` slot.
    std::unique_ptr<LockShader> shader;
    std::chrono::steady_clock::time_point lastShaderFrame;
    bool dmabufsAttached = false;
    // Index of the shader buffer the shell shows; the next frame goes into the other one.
    std::size_t shownBuffer = 1;
    Timer secondTimer;
    bool needsUpdate = true;
    bool needsLayout = true;
    bool dirty = true;
  };

  std::string readAll(int fd) {
    std::string text;
    char buffer[4096];
    for (ssize_t count = 0; (count = ::read(fd, buffer, sizeof(buffer))) > 0;) {
      text.append(buffer, static_cast<std::size_t>(count));
    }
    return text;
  }

  std::string readFileText(const std::filesystem::path& path) {
    std::ifstream file(path);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
  }

  SettingsMap readSettings(const toml::table* table) {
    SettingsMap settings;
    if (table != nullptr) {
      for (const auto& [key, node] : *table) {
        if (auto value = noctalia::config::readWidgetSettingValue(node); value.has_value()) {
          settings.emplace(std::string(key.str()), std::move(*value));
        }
      }
    }
    return settings;
  }

  void render(RenderContext& context, Slot& slot, float deltaMs) {
    if (slot.shader != nullptr) {
      LockShader& shader = *slot.shader;
      const auto& dmabufs = shader.dmabufs();
      bool sent = false;
      if (slot.server.dmabufAllowed() && dmabufs.size() == 2) {
        // Buffer ids 1 and 2 are the shader's two dmabufs; id 0 stays the memfd buffer.
        if (!slot.dmabufsAttached) {
          slot.dmabufsAttached = slot.server.attachDmabuf(1, dmabufs[0]) && slot.server.attachDmabuf(2, dmabufs[1]);
        }
        const std::size_t next = 1 - slot.shownBuffer;
        if (slot.dmabufsAttached && shader.draw(next) && slot.server.sendDmabufFrame(static_cast<std::uint32_t>(next) + 1)) {
          slot.shownBuffer = next;
          sent = true;
        }
      } else {
        const auto pixels = slot.server.frameBuffer(shader.width(), shader.height());
        sent = !pixels.empty() && shader.draw(0) && shader.read(0, pixels) && slot.server.sendFrame();
      }
      if (sent) {
        slot.lastShaderFrame = std::chrono::steady_clock::now();
      }
      // The shader leaves its own context current; widget code expects the renderer's.
      (void)context.backend().makeCurrentNoSurface();
      return;
    }
    if (slot.widget == nullptr) {
      return;
    }
    if (!context.makeCurrent(slot.target)) {
      return;
    }
    Renderer& renderer = slot.target.renderer();
    if (slot.widget->needsFrameTick()) {
      slot.widget->onFrameTick(deltaMs, renderer);
    }
    slot.animations.tick(deltaMs);
    if (std::exchange(slot.needsUpdate, false)) {
      slot.widget->update(renderer);
    }
    if (std::exchange(slot.needsLayout, false)) {
      slot.widget->layout(renderer);
    }
    (void)context.renderScene(slot.target, &slot.root);
    const auto pixels = slot.server.frameBuffer(slot.target.bufferWidth(), slot.target.bufferHeight());
    if (!pixels.empty() && context.readPixels(slot.target, pixels)) {
      slot.server.sendFrame();
      slot.dirty = false;
    }
  }

  [[nodiscard]] bool animating(const Slot& slot) {
    if (slot.shader != nullptr) {
      return true;
    }
    return slot.widget != nullptr && (slot.widget->needsFrameTick() || slot.animations.hasActive());
  }

} // namespace

int runLockHelper(int argc, char* argv[]) {
  int startFd = -1;
  for (int i = 2; i + 1 < argc; ++i) {
    if (std::strcmp(argv[i], "--start-fd") == 0) {
      startFd = std::atoi(argv[i + 1]);
    }
  }
  if (startFd < 0) {
    kLog.error("missing --start-fd");
    return 2;
  }

  toml::table start;
  try {
    start = toml::parse(readAll(startFd));
  } catch (const toml::parse_error& e) {
    kLog.error("bad start document: {}", e.description());
    return 2;
  }
  ::close(startFd);

  GlSharedContext shared;
  RenderContext context;
  try {
    shared.initializeSurfaceless();
    context.initialize(shared);
  } catch (const std::exception& e) {
    kLog.error("no GPU context: {}", e.what());
    return 1;
  }

  // Lock screen data only: clipboard, calendar and every action hook stay unset (DESIGN.md L5b detail).
  scripting::ScriptApiContext scriptApi;
  scriptApi.setDarkMode(start["dark"].value_or(true));
  scriptApi.setTimeFormat(start["time_format"].value_or(std::string{}));
  scriptApi.setDateFormat(start["date_format"].value_or(std::string{}));

  std::vector<std::filesystem::path> sources;
  if (const auto* array = start["plugin_sources"].as_array()) {
    for (const auto& node : *array) {
      if (const auto* path = node.as_string()) {
        sources.emplace_back(path->get());
      }
    }
  }
  scripting::PluginRegistry::instance().setSources(std::move(sources));
  scripting::PluginRegistry::instance().scan();

  std::unordered_map<std::string, SettingsMap> pluginSettings;
  if (const auto* table = start["plugin_settings"].as_table()) {
    for (const auto& [id, node] : *table) {
      pluginSettings.emplace(std::string(id.str()), readSettings(node.as_table()));
    }
  }
  const DesktopWidgetFactory factory(DesktopWidgetRuntimeServices{
      .scriptDeps = {.scriptApi = &scriptApi, .pluginSettings = &pluginSettings},
  });

  std::vector<std::unique_ptr<Slot>> slots;
  if (const auto* array = start["slot"].as_array()) {
    for (const auto& node : *array) {
      const auto* table = node.as_table();
      if (table == nullptr) {
        continue;
      }
      auto slot = std::make_unique<Slot>((*table)["fd"].value_or(-1));
      const float width = (*table)["width"].value_or(0.0F);
      const float height = (*table)["height"].value_or(0.0F);
      const float scale = (*table)["scale"].value_or(1.0F);
      const std::string type = (*table)["type"].value_or(std::string{});
      if (slot->server.fd() < 0 || width < 1.0F || height < 1.0F) {
        kLog.warn("skipping slot '{}'", type);
        continue;
      }
      if (type == "shader") {
        const SettingsMap settings = readSettings((*table)["settings"].as_table());
        const auto it = settings.find("path");
        const std::string* path = it != settings.end() ? std::get_if<std::string>(&it->second) : nullptr;
        const std::string source = path != nullptr ? readFileText(FileUtils::expandUserPath(*path)) : "";
        try {
          slot->shader = std::make_unique<LockShader>(
              shared, source, static_cast<std::uint32_t>(std::lround(width * scale)),
              static_cast<std::uint32_t>(std::lround(height * scale))
          );
        } catch (const std::exception& e) {
          // The slot stays open with no frames, so the shell keeps its placeholder and does not restart.
          kLog.warn("shader slot '{}': {}", path != nullptr ? *path : std::string{}, e.what());
        }
        (void)context.backend().makeCurrentNoSurface();
        slots.push_back(std::move(slot));
        continue;
      }
      slot->widget = factory.create(
          type, readSettings((*table)["settings"].as_table()), (*table)["content_scale"].value_or(1.0F)
      );
      if (slot->widget == nullptr) {
        kLog.warn("skipping slot '{}'", type);
        continue;
      }
      Slot* raw = slot.get();
      slot->target.createOffscreen(context);
      slot->target.setContentScale(scale);
      slot->target.setLogicalSize(static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height));
      slot->target.resize(
          static_cast<std::uint32_t>(std::lround(width * scale)), static_cast<std::uint32_t>(std::lround(height * scale))
      );
      slot->root.setSize(width, height);
      slot->widget->setAnimationManager(&slot->animations);
      slot->widget->setUpdateCallback([raw] { raw->needsUpdate = raw->dirty = true; });
      slot->widget->setLayoutCallback([raw] { raw->needsUpdate = raw->needsLayout = raw->dirty = true; });
      slot->widget->setRedrawCallback([raw] { raw->dirty = true; });
      slot->widget->setFrameTickRequestCallback([raw] { raw->dirty = true; });
      slot->widget->create();
      slot->widget->setBox(width, height);
      auto transform = ui::node({});
      transform->setAnimationManager(&slot->animations);
      slot->root.addChild(std::move(transform))->addChild(slot->widget->releaseRoot());
      if (slot->widget->wantsSecondTicks()) {
        slot->secondTimer.startRepeating(std::chrono::seconds(1), [raw] { raw->needsUpdate = raw->dirty = true; });
      }
      slots.push_back(std::move(slot));
    }
  }
  if (slots.empty()) {
    kLog.warn("no slots to render");
    return 0;
  }

  auto lastFrame = std::chrono::steady_clock::now();
  std::vector<pollfd> fds;
  while (true) {
    for (auto& fn : DeferredCall::takePending()) {
      if (fn) {
        fn();
      }
    }
    TimerManager::instance().tick();

    const auto now = std::chrono::steady_clock::now();
    const float deltaMs = std::chrono::duration<float, std::milli>(now - lastFrame).count();
    bool rendered = false;
    bool waitingAnimation = false;
    for (auto& slot : slots) {
      const bool shaderDue = slot->shader == nullptr || now - slot->lastShaderFrame >= std::chrono::milliseconds(kFrameIntervalMs);
      if (slot->server.requested() && (slot->dirty || animating(*slot)) && shaderDue) {
        render(context, *slot, deltaMs);
        rendered = true;
      }
      waitingAnimation = waitingAnimation || (animating(*slot) && slot->server.requested());
    }
    if (rendered) {
      lastFrame = now;
    }

    fds.clear();
    fds.push_back({.fd = STDIN_FILENO, .events = POLLIN, .revents = 0});
    fds.push_back({.fd = DeferredCall::wakeFd(), .events = POLLIN, .revents = 0});
    for (const auto& slot : slots) {
      fds.push_back({.fd = slot->server.fd(), .events = POLLIN, .revents = 0});
    }
    int timeout = TimerManager::instance().pollTimeoutMs();
    if (waitingAnimation) {
      timeout = timeout < 0 ? kFrameIntervalMs : std::min(timeout, kFrameIntervalMs);
    }
    if (::poll(fds.data(), fds.size(), timeout) < 0 && errno != EINTR) {
      return 1;
    }
    if ((fds[0].revents & (POLLERR | POLLNVAL)) != 0) {
      return 0;
    }
    if ((fds[0].revents & (POLLIN | POLLHUP)) != 0) {
      char byte = 0;
      if (::read(STDIN_FILENO, &byte, 1) <= 0) {
        return 0;
      }
    }
    if ((fds[1].revents & POLLIN) != 0) {
      DeferredCall::drainWakeFd();
    }
    for (std::size_t i = 0; i < slots.size(); ++i) {
      if (fds[i + 2].revents != 0 && !slots[i]->server.dispatch()) {
        kLog.info("a slot closed; exiting");
        return 0;
      }
      if (slots[i]->server.takeResend()) {
        slots[i]->dirty = true;
      }
    }
  }
}
