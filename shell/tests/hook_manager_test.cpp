#include "core/process/async_process_manager.h"
#include "core/process/process.h"
#include "hooks/hook_manager.h"
#include "tests/test_check.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <unistd.h>
#include <string>
#include <string_view>
#include <vector>

std::string_view hookKindKey(HookKind kind) {
  if (kind == HookKind::WallpaperChanged) {
    return "wallpaper_changed";
  }
  return "unknown";
}

namespace {

  const char* envOrEmpty(const char* name) {
    if (const char* value = std::getenv(name)) {
      return value;
    }
    return "";
  }

  // The follow-up action must run only after every hook command has finished.
  bool asyncHookFinishesBeforeAction() {
    const std::string marker = "/tmp/noctalia_hook_marker_" + std::to_string(::getpid());
    std::filesystem::remove(marker);

    HookManager hooks;
    HooksConfig config;
    config.commands[static_cast<std::size_t>(HookKind::WallpaperChanged)] = {"sleep 0.2; touch " + marker};
    hooks.reload(config);
    hooks.setAsyncCommandRunner([](const std::string& command, std::function<void(bool)> done) {
      process::RunCallbacks callbacks{.onExit = [done](process::RunResult result) { done(static_cast<bool>(result)); }};
      if (!process::runAsync(std::vector<std::string>{"/bin/sh", "-c", command}, std::move(callbacks))) {
        done(false);
      }
    });

    bool finished = false;
    bool hookOk = false;
    bool markerSeenByAction = false;
    hooks.fireAsync(HookKind::WallpaperChanged, [&](bool ok) {
      markerSeenByAction = std::filesystem::exists(marker);
      hookOk = ok;
      finished = true;
    });

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!finished && std::chrono::steady_clock::now() < deadline) {
      process::AsyncProcessManager::instance().pumpOnce(std::chrono::milliseconds(20));
    }
    std::filesystem::remove(marker);
    return finished && hookOk && markerSeenByAction;
  }

} // namespace

int main() {
  TEST_CHECK(asyncHookFinishesBeforeAction());

  constexpr const char* kPathName = "NOCTALIA_WALLPAPER_PATH";
  constexpr const char* kConnectorName = "NOCTALIA_WALLPAPER_CONNECTOR";

  ::unsetenv(kPathName);
  ::unsetenv(kConnectorName);

  HookManager hooks;
  HooksConfig config;
  config.commands[static_cast<std::size_t>(HookKind::WallpaperChanged)] = {"record-wallpaper-hook"};
  hooks.reload(config);

  std::vector<std::string> commands;
  std::string pathSeen;
  std::string connectorSeen;
  hooks.setCommandRunner([&](const std::string& command) {
    commands.push_back(command);
    pathSeen = envOrEmpty(kPathName);
    connectorSeen = envOrEmpty(kConnectorName);
    return true;
  });

  hooks.fire(HookKind::WallpaperChanged, {{kPathName, "/tmp/noctalia test/wallpaper.png"}, {kConnectorName, "DP-1"}});

  TEST_CHECK(commands.size() == 1);
  TEST_CHECK(commands[0] == "record-wallpaper-hook");
  TEST_CHECK(pathSeen == "/tmp/noctalia test/wallpaper.png");
  TEST_CHECK(connectorSeen == "DP-1");
  TEST_CHECK(std::getenv(kPathName) == nullptr);
  TEST_CHECK(std::getenv(kConnectorName) == nullptr);

  return 0;
}
