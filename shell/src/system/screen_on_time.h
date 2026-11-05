#pragma once

#include "dbus/upower/upower_service.h"
#include "protocols/screen_activity_log.h"

#include <cstdint>
#include <optional>
#include <span>

// Screen-on time (SOT) of Settings > Battery: measured from the compositor's state spans and UPower's charge history.
enum class SotPeriod : std::uint8_t { SinceUnplugged, SinceFullCharge };

struct ScreenOnTime {
  std::int64_t start = 0; // UNIX seconds
  std::int64_t onSeconds = 0;
  std::int64_t idleSeconds = 0;
  std::int64_t offSeconds = 0;   // screen off, awake
  std::int64_t sleepSeconds = 0; // suspended
  double usedPercent = 0.0;      // battery drop over the period's discharging time; top-ups do not offset it
  // Measured drain over history segments spent mostly (>= 80 %) with the screen on, or mostly off or asleep; 0 when
  // no such segment exists yet.
  double screenOnDrainPerHour = 0.0;
  double screenOffDrainPerHour = 0.0;
};

// Start of the period: the last unplug (only while discharging now), or the first discharge after the last full
// charge. `points` in any order.
[[nodiscard]] std::optional<std::int64_t> sotPeriodStart(std::span<const UPowerHistoryPoint> points, SotPeriod period);

[[nodiscard]] ScreenOnTime screenOnTime(
    std::span<const ScreenActivityLog::Span> spans, std::span<const UPowerHistoryPoint> points, std::int64_t start,
    std::int64_t now
);
