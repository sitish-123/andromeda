#pragma once

#include "system/activity_service.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <string_view>

// Shell features that make a background helper redundant (PLAN-background-activity.md, Duplicate detection).
enum class ShellFeature : std::uint8_t {
  None,
  Bluetooth,
  Network,
  Sound,
  Battery,
  Notifications,
  Link,
  PolkitAgent,
  Clipboard,
  NightLight,
  Drives,
  Plasma, // KDE session helpers with no Plasma session running
};

// The shell feature an app ID or unit name duplicates, or ShellFeature::None.
[[nodiscard]] ShellFeature duplicateFeatureFor(std::string_view appIdOrUnit);
// Session plumbing that is never flagged: the shell, audio, portals, D-Bus, agents, systemd itself.
[[nodiscard]] bool neverFlag(std::string_view unitName);

struct BackgroundThresholds {
  std::chrono::minutes minUptime{30};
  std::uint64_t memoryBytes = 200ULL * 1024 * 1024;
  double cpuAveragePercent = 1.0; // share of all CPUs over the unit's lifetime
};

struct BackgroundContext {
  std::uint64_t nowUsec = 0; // CLOCK_REALTIME
  int cpuCount = 1;
  std::string homeDir;
  BackgroundThresholds thresholds;
  std::set<std::string, std::less<>> trayUnits;           // units that own a StatusNotifierItem
  std::set<ShellFeature> enabledFeatures;                 // a duplicate counts only when the shell feature is on
  std::function<bool(const ActivityRow&)> hasTerminal;    // per-PID reads: called only for flag candidates
};

struct BackgroundVerdict {
  enum class Finding : std::uint8_t { None, UsesResources, Duplicate };
  bool background = false; // no window, no tray icon, no terminal
  Finding finding = Finding::None;
  ShellFeature duplicates = ShellFeature::None;
  bool notify = false; // a finding the user installed or enabled (unit file under ~/.config or /etc, or preset disabled)
  std::uint64_t uptimeSec = 0;
  double cpuAveragePercent = 0.0;
};

// fragmentPath and unitFilePreset belong to the row's first unit.
[[nodiscard]] BackgroundVerdict classifyBackground(
    const ActivityRow& row, std::string_view fragmentPath, std::string_view unitFilePreset, const BackgroundContext& context
);
