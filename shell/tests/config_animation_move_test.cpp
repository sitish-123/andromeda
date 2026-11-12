// [shell.animation] moved to the compositor's animation.enabled and animation.speed. Settings values in the overrides
// file move to the compositor once; a config file that still has the section loads.
#include "config/config_service.h"
#include "config/config_types.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <print>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

  int g_failures = 0;

  void expect(bool condition, const char* message) {
    if (!condition) {
      std::println(stderr, "config_animation_move: FAIL: {}", message);
      ++g_failures;
    }
  }

  void writeFile(const std::filesystem::path& path, std::string_view content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::trunc);
    out << content;
  }

  std::string readFile(const std::filesystem::path& path) {
    std::ifstream in(path);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  }

  using Writes = std::vector<std::pair<std::string, std::string>>;

  // The compositor end of dsk_settings_manager_v1: it holds values, and a set lands only when it accepts it.
  struct Compositor {
    std::map<std::string, std::string, std::less<>> values{{"animation.enabled", "true"}, {"animation.speed", "1"}};
    Writes received;

    // One settings batch; returns the sets the shell sent in reply.
    Writes batch(ConfigService& config, std::string_view failure = {}) {
      Writes sent;
      config.moveLegacyAnimationSettings(
          [this](std::string_view key) {
            const auto it = values.find(key);
            return it != values.end() ? it->second : std::string();
          },
          failure, [&sent](const std::string& key, const std::string& value) { sent.emplace_back(key, value); }
      );
      received.insert(received.end(), sent.begin(), sent.end());
      return sent;
    }

    void accept() {
      for (const auto& [key, value] : received) {
        values.insert_or_assign(key, value);
      }
      received.clear();
    }
  };

} // namespace

int main() {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / ("noctalia-animation-move-" + std::to_string(::getpid()));
  std::filesystem::remove_all(root);
  const std::filesystem::path configFile = root / "config" / "andromeda" / "shell" / "config.toml";
  const std::filesystem::path overridesFile = root / "state" / "andromeda" / "shell" / "overrides.toml";
  ::setenv("ANDROMEDA_CONFIG_HOME", (root / "config").c_str(), 1);
  ::setenv("XDG_STATE_HOME", (root / "state").c_str(), 1);
  ::unsetenv("ANDROMEDA_STATE_HOME");
  ::unsetenv("NOCTALIA_STATE_HOME");

  writeFile(configFile, "[dock]\nenabled = false\n\n[shell.animation]\nenabled = false\nspeed = 3.0\n");
  writeFile(overridesFile, "[shell.animation]\nenabled = false\nspeed = 2.0\n\n[dock]\nicon_size = 40\n");
  const Writes moved{{"animation.enabled", "false"}, {"animation.speed", "2"}};
  {
    ConfigService config;
    expect(!config.config().dock.enabled, "a config file with [shell.animation] did not load");
    expect(config.config().dock.iconSize == 40, "an overrides file with [shell.animation] did not load");
    expect(
        std::ranges::contains(config.legacyConfigIssues(), "shell.animation", &noctalia::config::LegacyConfigIssue::path),
        "the config file's [shell.animation] raised no reminder"
    );

    Compositor refusing;
    expect(refusing.batch(config) == moved, "the overrides values were not sent to the compositor keys");
    expect(readFile(overridesFile).contains("speed = 2"), "the section went before the compositor confirmed it");
    expect(refusing.batch(config, "cannot write the settings file").empty(), "a refusal was answered with a resend");
    expect(readFile(overridesFile).contains("speed = 2"), "a refused move dropped the section");
  }
  {
    ConfigService restarted;
    Compositor accepting;
    expect(accepting.batch(restarted) == moved, "a restart after a refusal did not send the values again");
    accepting.accept();
    expect(accepting.batch(restarted).empty(), "a confirmed move sent again");
    expect(!readFile(overridesFile).contains("animation"), "a confirmed move kept [shell.animation]");
    expect(restarted.config().dock.iconSize == 40, "moving the section dropped other overrides");
    expect(accepting.batch(restarted).empty(), "a finished move sent again");
  }
  {
    ConfigService moved;
    Compositor compositor;
    expect(compositor.batch(moved).empty(), "a restart after the move sent again");
  }

  for (const char* section : {"[shell.animation]\nenabled = true\nspeed = 1.0\n", "[shell.animation]\nspeed = nan\n"}) {
    writeFile(overridesFile, section);
    ConfigService config;
    Compositor compositor;
    expect(compositor.batch(config).empty(), "a default or non-finite value was sent to the compositor");
    expect(!readFile(overridesFile).contains("animation"), "a section with nothing to move stayed");
  }

  std::filesystem::remove_all(root);
  if (g_failures != 0) {
    std::println(stderr, "config_animation_move: {} failure(s)", g_failures);
    return 1;
  }
  std::println("config_animation_move: ok");
  return 0;
}
