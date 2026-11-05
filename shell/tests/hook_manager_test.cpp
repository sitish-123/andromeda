#include "core/process/async_process_manager.h"
#include "core/process/process.h"
#include "hooks/hook_manager.h"
#include "tests/test_check.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
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

  // Executables in <dir>/wallpaper-changed.d run one after another in name order as units with the event data; a
  // failure is kept; fireAsync's done waits for them.
  bool dirHooksRunAsUnits() {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / ("andromeda_hooks_" + std::to_string(::getpid()));
    fs::remove_all(dir);
    fs::create_directories(dir / "wallpaper-changed.d");
    for (const char* name : {"20-second", "10-first", "30-not-executable"}) {
      std::ofstream(dir / "wallpaper-changed.d" / name) << "#!/bin/sh\n";
      if (std::string_view(name) != "30-not-executable") {
        fs::permissions(dir / "wallpaper-changed.d" / name, fs::perms::owner_all);
      }
    }
    HookManager hooks;
    std::vector<std::vector<std::string>> runs;
    std::vector<std::function<void(bool)>> pending;
    hooks.setUnitRunner(
        [&](std::vector<std::string> argv, std::function<void(bool)> done) {
          runs.push_back(std::move(argv));
          pending.push_back(std::move(done));
        },
        dir
    );
    hooks.setAsyncCommandRunner([](const std::string&, std::function<void(bool)> done) { done(true); });
    auto has = [](const std::vector<std::string>& argv, std::string_view arg) {
      return std::ranges::find(argv, arg) != argv.end();
    };

    bool ok = true;
    hooks.fire(HookKind::WallpaperChanged, {{"NOCTALIA_WALLPAPER_PATH", "/w.png"}});
    ok = ok && runs.size() == 1 && runs[0].back().ends_with("10-first");
    ok = ok && has(runs[0], "--setenv=NOCTALIA_WALLPAPER_PATH=/w.png") && has(runs[0], "--property=RuntimeMaxSec=10");
    pending[0](true);
    ok = ok && runs.size() == 2 && runs[1].back().ends_with("20-second");
    pending[1](false);
    const auto failures = hooks.dirHookFailures();
    ok = ok && runs.size() == 2 && failures.size() == 1 && failures[0].script == "20-second" &&
         failures[0].event == "wallpaper-changed";

    bool asyncDone = false;
    hooks.fireAsync(HookKind::WallpaperChanged, [&](bool) { asyncDone = true; });
    ok = ok && runs.size() == 3 && !asyncDone;
    pending[2](true);
    ok = ok && !asyncDone;
    pending[3](true);
    ok = ok && asyncDone;
    fs::remove_all(dir);
    return ok;
  }

} // namespace

int main() {
  TEST_CHECK(asyncHookFinishesBeforeAction());
  TEST_CHECK(dirHooksRunAsUnits());

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
