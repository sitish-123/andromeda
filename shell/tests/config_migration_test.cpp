#include "config/config_migrations.h"
#include "core/toml.h"

#include <algorithm>
#include <cstdint>
#include <format>
#include <print>
#include <string>
#include <string_view>

namespace {

  int g_failures = 0;
  int g_syntheticMigrationApplications = 0;

  void countSyntheticMigration(toml::table&, noctalia::config::schema::Diagnostics&) {
    ++g_syntheticMigrationApplications;
  }

  void expect(bool condition, std::string_view message) {
    if (!condition) {
      std::println(stderr, "config_migration_test: FAIL: {}", message);
      ++g_failures;
    }
  }

  bool hasIssuePath(const noctalia::config::LegacyConfigIssues& issues, std::string_view path) {
    return std::ranges::any_of(issues, [path](const auto& issue) { return issue.path == path; });
  }

  void checkLockscreenThemeMigration() {
    // style maps to theme: classic -> andromeda, ryoku -> ryoku; the old key goes.
    toml::table classic = toml::parse("[lockscreen]\nstyle = \"classic\"\n");
    noctalia::config::LegacyConfigIssues issues;
    noctalia::config::normalizeLegacyConfig(classic, issues);
    expect(classic["lockscreen"]["theme"].value<std::string>() == "andromeda", "classic did not become andromeda");
    expect(!classic["lockscreen"]["style"], "style was kept next to theme");
    expect(hasIssuePath(issues, "lockscreen.style"), "style migration raised no issue");

    // The merged config never invents a theme: an unset key keeps the Andromeda default.
    toml::table unset = toml::parse("[bar.main]\nradius = 12\n");
    noctalia::config::LegacyConfigIssues unsetIssues;
    noctalia::config::normalizeLegacyConfig(unset, unsetIssues);
    expect(!unset["lockscreen"], "normalization invented a lockscreen theme");
    expect(unsetIssues.empty(), "an unset theme raised an issue");

    // An explicit theme wins over a leftover style.
    toml::table both = toml::parse("[lockscreen]\ntheme = \"andromeda\"\nstyle = \"ryoku\"\n");
    noctalia::config::LegacyConfigIssues bothIssues;
    noctalia::config::normalizeLegacyConfig(both, bothIssues);
    expect(both["lockscreen"]["theme"].value<std::string>() == "andromeda", "style overrode an explicit theme");

    // The Settings overrides file migrates without inventing a theme.
    toml::table sidecar = toml::parse("[lockscreen]\nstyle = \"classic\"\n");
    noctalia::config::schema::Diagnostics diag;
    (void)noctalia::config::applyPendingConfigMigrations(sidecar, 16, diag);
    expect(sidecar["lockscreen"]["theme"].value<std::string>() == "andromeda", "sidecar style was not migrated");
    // An overrides file from an older install (it records config_version) keeps the Ryoku look it had.
    toml::table oldSidecar = toml::parse("config_version = 16\n[dock]\nradius = 8\n");
    (void)noctalia::config::applyPendingConfigMigrations(oldSidecar, 16, diag);
    expect(oldSidecar["lockscreen"]["theme"].value<std::string>() == "ryoku", "an older install lost the Ryoku look");
    // A fresh overrides file (no version yet, all migrations run) keeps the Andromeda default.
    toml::table freshSidecar = toml::parse("[dock]\nradius = 8\n");
    (void)noctalia::config::applyPendingConfigMigrations(freshSidecar, 0, diag);
    expect(!freshSidecar["lockscreen"], "a fresh install was moved to Ryoku");
  }

  void checkVersionGating() {
    toml::table legacy = toml::parse("[lockscreen]\nstyle = \"classic\"\n");
    noctalia::config::schema::Diagnostics diag;
    const auto stored = noctalia::config::storedConfigVersion(legacy, diag);
    expect(stored == 0, "missing config_version was not treated as legacy version 0");
    const int applied = noctalia::config::applyPendingConfigMigrations(legacy, stored.value_or(0), diag);
    expect(applied == noctalia::config::currentConfigVersion(), "pending migration did not reach current version");
    expect(legacy["lockscreen"]["theme"].value<std::string>() == "andromeda", "sidecar migration did not run");

    toml::table current = toml::parse(std::format(
        "config_version = {}\n[lockscreen]\nstyle = \"classic\"\n", noctalia::config::currentConfigVersion()
    ));
    noctalia::config::schema::Diagnostics currentDiag;
    const auto currentStored = noctalia::config::storedConfigVersion(current, currentDiag);
    expect(currentStored == noctalia::config::currentConfigVersion(), "current config_version was not read");
    (void)noctalia::config::applyPendingConfigMigrations(current, currentStored.value_or(0), currentDiag);
    expect(!current["lockscreen"]["theme"], "current sidecar replayed a historical migration");

    toml::table invalid = toml::parse("config_version = \"one\"");
    noctalia::config::schema::Diagnostics invalidDiag;
    expect(
        !noctalia::config::storedConfigVersion(invalid, invalidDiag).has_value(), "invalid config_version was accepted"
    );
    expect(invalidDiag.hasErrors(), "invalid config_version did not produce an error");
    expect(invalidDiag.hasFatalErrors(), "invalid config_version was not document-fatal");

    toml::table future = toml::parse("config_version = 999");
    noctalia::config::schema::Diagnostics futureDiag;
    expect(
        !noctalia::config::storedConfigVersion(future, futureDiag).has_value(), "future config_version was accepted"
    );
    expect(futureDiag.hasErrors(), "future config_version did not produce an error");
    expect(futureDiag.hasFatalErrors(), "future config_version was not document-fatal");

    noctalia::config::schema::Diagnostics baseline;
    baseline.componentError("widget.clock.timezone", "widget.clock", "unknown timezone", "clock.timezone.unknown");
    noctalia::config::schema::Diagnostics candidate = baseline;
    candidate.error("accessibility.ui_scale", "expected a number", "config.type.number");
    const auto introduced = candidate.introducedErrorsComparedTo(baseline);
    expect(introduced.entries.size() == 1, "diagnostic comparison did not isolate the new error");
    expect(
        introduced.entries.front().path == "accessibility.ui_scale", "diagnostic comparison returned the wrong error"
    );
  }

  void checkReminderFingerprint() {
    const noctalia::config::LegacyConfigIssues first = {{1, "bar.main", "message"}};
    const noctalia::config::LegacyConfigIssues reordered = {
        {1, "bar.second", "message"},
        {1, "bar.main", "different display message"},
    };
    const noctalia::config::LegacyConfigIssues sameReordered = {
        {1, "bar.main", "message"},
        {1, "bar.second", "message"},
    };

    const std::string firstFingerprint = noctalia::config::legacyConfigIssueFingerprint(first);
    const std::string expandedFingerprint = noctalia::config::legacyConfigIssueFingerprint(reordered);
    expect(
        expandedFingerprint == noctalia::config::legacyConfigIssueFingerprint(sameReordered),
        "fingerprint depends on issue ordering or display message"
    );
    expect(
        noctalia::config::legacyConfigFingerprintHasNewIssues(expandedFingerprint, firstFingerprint),
        "new issue was not detected"
    );
    expect(
        !noctalia::config::legacyConfigFingerprintHasNewIssues(firstFingerprint, expandedFingerprint),
        "removing an issue was treated as introducing one"
    );

    constexpr std::int64_t kStart = 1'000'000;
    expect(
        !noctalia::config::legacyConfigReminderIntervalElapsed(
            kStart + noctalia::config::kLegacyConfigReminderIntervalSeconds - 1, kStart
        ),
        "reminder became due before three days"
    );
    expect(
        noctalia::config::legacyConfigReminderIntervalElapsed(
            kStart + noctalia::config::kLegacyConfigReminderIntervalSeconds, kStart
        ),
        "reminder was not due at three days"
    );
    expect(
        noctalia::config::legacyConfigReminderIntervalElapsed(kStart - 1, kStart),
        "backward clock change did not make the reminder due"
    );
  }

  void checkRegistryOrdering() {
    int previousVersion = 0;
    for (const auto& migration : noctalia::config::configMigrations()) {
      expect(migration.toVersion > previousVersion, "migration registry is out of order");
      expect(!migration.summary.empty(), "migration registry entry has no summary");
      expect(migration.apply != nullptr, "migration registry entry has no apply function");
      previousVersion = migration.toVersion;
    }
    expect(
        previousVersion == noctalia::config::currentConfigVersion(),
        "current config version does not match the registry"
    );
    // Stored files record version 17; a lower current version would reject them as "newer than supported".
    expect(noctalia::config::currentConfigVersion() >= 17, "current config version fell below stored files");
  }

  void checkLargeCurrentRegistrySkipsBodies() {
    std::vector<noctalia::config::ConfigMigration> migrations;
    migrations.reserve(100);
    for (int version = 1; version <= 100; ++version) {
      migrations.push_back({
          .toVersion = version,
          .summary = "synthetic migration",
          .apply = countSyntheticMigration,
      });
    }

    toml::table root;
    noctalia::config::schema::Diagnostics diag;
    g_syntheticMigrationApplications = 0;
    const int current = noctalia::config::applyPendingConfigMigrations(root, 100, diag, migrations);
    expect(current == 100, "synthetic current version changed");
    expect(g_syntheticMigrationApplications == 0, "current sidecar executed historical migration bodies");

    const int upgraded = noctalia::config::applyPendingConfigMigrations(root, 99, diag, migrations);
    expect(upgraded == 100, "synthetic upgrade did not reach the current version");
    expect(g_syntheticMigrationApplications == 1, "synthetic upgrade did not execute exactly one pending body");
  }

} // namespace

int main() {
  checkLockscreenThemeMigration();
  checkVersionGating();
  checkReminderFingerprint();
  checkRegistryOrdering();
  checkLargeCurrentRegistrySkipsBodies();

  if (g_failures == 0) {
    std::println("config_migration_test: all checks passed");
  }
  return g_failures == 0 ? 0 : 1;
}
