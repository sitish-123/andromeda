#pragma once

#include "config/schema/diagnostics.h"
#include "core/toml.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace noctalia::config {

  inline constexpr std::string_view kConfigVersionKey = "config_version";

  struct ConfigMigration {
    int toVersion;
    std::string_view summary;
    void (*apply)(toml::table& root, schema::Diagnostics& diag);
  };

  struct LegacyConfigIssue {
    int migrationVersion;
    std::string path;
    std::string message;
    schema::SourceOrigin origin;

    bool operator==(const LegacyConfigIssue&) const = default;
  };

  using LegacyConfigIssues = std::vector<LegacyConfigIssue>;

  [[nodiscard]] const std::vector<ConfigMigration>& configMigrations();
  [[nodiscard]] int currentConfigVersion();
  [[nodiscard]] std::optional<int> storedConfigVersion(const toml::table& root, schema::Diagnostics& diag);
  [[nodiscard]] int applyPendingConfigMigrations(
      toml::table& root, int storedVersion, schema::Diagnostics& diag,
      std::span<const ConfigMigration> migrations = configMigrations()
  );

  void normalizeLegacyConfig(toml::table& root, LegacyConfigIssues& issues);

  // [shell.animation] moved to the compositor's animation.enabled and animation.speed. Returns the compositor settings
  // (key, value) for the values in `root` that differ from the old shell defaults.
  [[nodiscard]] std::vector<std::pair<std::string, std::string>> legacyShellAnimationSettings(const toml::table& root);
  // Removes [shell.animation]; true when `root` had it.
  bool eraseLegacyShellAnimation(toml::table& root);

  [[nodiscard]] std::string legacyConfigIssueFingerprint(const LegacyConfigIssues& issues);
  [[nodiscard]] bool
  legacyConfigFingerprintHasNewIssues(std::string_view currentFingerprint, std::string_view previousFingerprint);
  [[nodiscard]] bool
  legacyConfigReminderIntervalElapsed(std::int64_t nowEpochSeconds, std::int64_t previousEpochSeconds);

  inline constexpr std::int64_t kLegacyConfigReminderIntervalSeconds = 3 * 24 * 60 * 60;

} // namespace noctalia::config
