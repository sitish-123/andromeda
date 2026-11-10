// The overrides watcher skips only the echo of the shell's own write. An external edit that lands in
// the same inotify batch as that echo, or after an echo that never arrived, must still reload.

#include "config/config_service.h"
#include "core/timer_manager.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <poll.h>
#include <print>
#include <string>
#include <unistd.h>

namespace {

  int g_failures = 0;

  void expect(bool condition, const char* message) {
    if (!condition) {
      std::println(stderr, "config_watch_echo: FAIL: {}", message);
      ++g_failures;
    }
  }

  void writeFile(const std::filesystem::path& path, std::string_view content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::trunc);
    out << content;
  }

  // One rename event, as editors save: a truncate-then-write edit spreads over batches.
  void writeFileAtomic(const std::filesystem::path& path, std::string_view content) {
    const auto temp = path.string() + ".tmp";
    writeFile(temp, content);
    std::filesystem::rename(temp, path);
  }

  std::filesystem::path findOverrides(const std::filesystem::path& stateRoot) {
    for (const auto& entry : std::filesystem::recursive_directory_iterator(stateRoot)) {
      if (entry.path().filename() == "overrides.toml") {
        return entry.path();
      }
    }
    return {};
  }

  bool waitFor(ConfigService& config, const std::function<bool()>& done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
      pollfd descriptor{.fd = config.watchFd(), .events = POLLIN};
      if (::poll(&descriptor, 1, 10) > 0) {
        config.checkReload();
      }
      TimerManager::instance().tick();
      if (done()) {
        return true;
      }
    }
    return false;
  }

  bool enabled(const ConfigService& config, std::string_view id) {
    return std::ranges::contains(config.config().plugins.enabled, id);
  }

  void drainWatch(ConfigService& config) {
    std::array<char, 4096> buffer{};
    while (true) {
      pollfd descriptor{.fd = config.watchFd(), .events = POLLIN};
      if (::poll(&descriptor, 1, 50) <= 0 || ::read(config.watchFd(), buffer.data(), buffer.size()) <= 0) {
        return;
      }
    }
  }

} // namespace

int main() {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / ("noctalia-watch-echo-" + std::to_string(::getpid()));
  std::filesystem::remove_all(root);
  writeFile(root / "config" / "andromeda" / "shell" / "config.toml", "[shell]\n");
  ::setenv("ANDROMEDA_CONFIG_HOME", (root / "config").c_str(), 1);
  ::setenv("XDG_STATE_HOME", (root / "state").c_str(), 1);

  ConfigService config;
  expect(config.watchFd() >= 0, "inotify watch is set up");

  // External edit in the same batch as the echo of a setter write.
  config.setPluginEnabled("me/own", true);
  const auto overrides = findOverrides(root / "state");
  expect(!overrides.empty(), "setter wrote overrides.toml");
  writeFileAtomic(overrides, "[plugins]\nenabled = [\"ext/same-batch\"]\n");
  expect(waitFor(config, [&] { return enabled(config, "ext/same-batch"); }), "same-batch external edit reloads");

  // External edit after a setter write whose echo never reaches the watcher.
  config.setPluginEnabled("me/second", true);
  drainWatch(config);
  writeFileAtomic(overrides, "[plugins]\nenabled = [\"ext/after-lost-echo\"]\n");
  expect(
      waitFor(config, [&] { return enabled(config, "ext/after-lost-echo"); }), "external edit after a lost echo reloads"
  );

  std::filesystem::remove_all(root);
  return g_failures == 0 ? 0 : 1;
}
