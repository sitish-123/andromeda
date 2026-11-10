#include "config/config_migrations.h"

#include <algorithm>
#include <cstdint>
#include <format>
#include <limits>
#include <set>
#include <string>
#include <string_view>

namespace noctalia::config {
  namespace {

    // Versions 1-16 upgraded upstream Noctalia schemas and are retired; numbering continues so stored files stay valid.
    constexpr int kLockscreenThemeMigrationVersion = 17;

    // lockscreen.style (ryoku | classic) became lockscreen.theme (ryoku | andromeda). keepRyokuWhenUnset writes ryoku
    // when neither key is set, for installs from before the theme key.
    template <typename OnChanged>
    void migrateLockscreenTheme(toml::table& root, bool keepRyokuWhenUnset, OnChanged&& onChanged) {
      auto* section = root["lockscreen"].as_table();
      const auto style = section != nullptr ? (*section)["style"].value<std::string>() : std::nullopt;
      if (section != nullptr && section->contains("theme")) {
        if (section->erase("style") > 0) {
          onChanged("lockscreen.style");
        }
        return;
      }
      if (!style.has_value() && !keepRyokuWhenUnset) {
        return;
      }
      if (section == nullptr) {
        root.insert_or_assign("lockscreen", toml::table{});
        section = root["lockscreen"].as_table();
      }
      section->insert_or_assign("theme", style.value_or("ryoku") == "classic" ? "andromeda" : "ryoku");
      if (section->erase("style") > 0) {
        onChanged("lockscreen.style");
      }
    }

    void migrateLockscreenThemeSidecar(toml::table& root, schema::Diagnostics& diag) {
      // A file that records an older config_version comes from an install that had the Ryoku look; a fresh overrides
      // file has no version yet and keeps the Andromeda default.
      migrateLockscreenTheme(root, root.contains(kConfigVersionKey), [&diag](const std::string& path) {
        diag.warn(path, "lockscreen style is now theme");
      });
    }

    std::uint64_t stableIssueHash(int migrationVersion, std::string_view path) {
      constexpr std::uint64_t kOffset = 14695981039346656037ULL;
      constexpr std::uint64_t kPrime = 1099511628211ULL;
      std::uint64_t hash = kOffset;
      const auto append = [&hash](std::string_view value) {
        for (const unsigned char byte : value) {
          hash ^= byte;
          hash *= kPrime;
        }
      };
      append(std::to_string(migrationVersion));
      append(":");
      append(path);
      return hash;
    }

    std::set<std::string> splitFingerprint(std::string_view fingerprint) {
      std::set<std::string> entries;
      std::size_t start = 0;
      while (start < fingerprint.size()) {
        const std::size_t end = fingerprint.find(',', start);
        const std::size_t length = end == std::string_view::npos ? fingerprint.size() - start : end - start;
        if (length > 0) {
          entries.emplace(fingerprint.substr(start, length));
        }
        if (end == std::string_view::npos) {
          break;
        }
        start = end + 1;
      }
      return entries;
    }

  } // namespace

  const std::vector<ConfigMigration>& configMigrations() {
    static const std::vector<ConfigMigration> migrations = {
        {
            .toVersion = kLockscreenThemeMigrationVersion,
            .summary = "lockscreen: style (ryoku | classic) is now theme (ryoku | andromeda)",
            .apply = migrateLockscreenThemeSidecar,
        },
    };
    return migrations;
  }

  int currentConfigVersion() {
    const auto& migrations = configMigrations();
    return migrations.empty() ? 0 : migrations.back().toVersion;
  }

  std::optional<int> storedConfigVersion(const toml::table& root, schema::Diagnostics& diag) {
    const toml::node* node = root.get(kConfigVersionKey);
    if (node == nullptr) {
      return 0;
    }

    const auto value = node->value<std::int64_t>();
    if (!value.has_value()) {
      diag.fatal(std::string(kConfigVersionKey), "expected a non-negative integer", "config.version.invalid");
      return std::nullopt;
    }
    if (*value < 0 || *value > std::numeric_limits<int>::max()) {
      diag.fatal(std::string(kConfigVersionKey), "expected a non-negative integer", "config.version.invalid");
      return std::nullopt;
    }

    const int version = static_cast<int>(*value);
    if (version > currentConfigVersion()) {
      diag.fatal(
          std::string(kConfigVersionKey),
          std::format("version {} is newer than supported version {}", version, currentConfigVersion()),
          "config.version.unsupported"
      );
      return std::nullopt;
    }
    return version;
  }

  int applyPendingConfigMigrations(
      toml::table& root, int storedVersion, schema::Diagnostics& diag, std::span<const ConfigMigration> migrations
  ) {
    int appliedVersion = storedVersion;
    for (const ConfigMigration& migration : migrations) {
      if (migration.toVersion <= storedVersion) {
        continue;
      }
      migration.apply(root, diag);
      appliedVersion = migration.toVersion;
    }
    return appliedVersion;
  }

  void normalizeLegacyConfig(toml::table& root, LegacyConfigIssues& issues) {
    migrateLockscreenTheme(root, false, [&issues](const std::string& path) {
      issues.push_back({
          .migrationVersion = kLockscreenThemeMigrationVersion,
          .path = path,
          .message = "lockscreen style is deprecated; use theme = \"ryoku\" or \"andromeda\"",
      });
    });
  }

  std::string legacyConfigIssueFingerprint(const LegacyConfigIssues& issues) {
    std::set<std::string> entries;
    for (const LegacyConfigIssue& issue : issues) {
      entries.insert(std::format("{:016x}", stableIssueHash(issue.migrationVersion, issue.path)));
    }

    std::string fingerprint;
    for (const std::string& entry : entries) {
      if (!fingerprint.empty()) {
        fingerprint.push_back(',');
      }
      fingerprint += entry;
    }
    return fingerprint;
  }

  bool legacyConfigFingerprintHasNewIssues(std::string_view currentFingerprint, std::string_view previousFingerprint) {
    const std::set<std::string> current = splitFingerprint(currentFingerprint);
    const std::set<std::string> previous = splitFingerprint(previousFingerprint);
    return std::ranges::any_of(current, [&previous](const std::string& entry) { return !previous.contains(entry); });
  }

  bool legacyConfigReminderIntervalElapsed(std::int64_t nowEpochSeconds, std::int64_t previousEpochSeconds) {
    return previousEpochSeconds > nowEpochSeconds
        || nowEpochSeconds - previousEpochSeconds >= kLegacyConfigReminderIntervalSeconds;
  }

} // namespace noctalia::config
