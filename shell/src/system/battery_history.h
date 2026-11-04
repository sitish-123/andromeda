#pragma once

#include "dbus/upower/upower_service.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

// Battery level chart data for [now - span, now].
struct BatteryLevelSeries {
  // Fraction of the range where the first sample sits; `level` covers [startFraction, 1].
  float startFraction = 1.0F;
  // Charge 0..1 at the end of each slot from startFraction on; the last known value carries into slots without one.
  std::vector<float> level;
  // Charging time as [from, to) fractions of the range, merged and in order.
  std::vector<std::pair<float, float>> charging;
};

// `points` in any order (UPower sends newest first). Empty `level` when no point falls before `now`.
[[nodiscard]] BatteryLevelSeries
batteryLevelSeries(std::span<const UPowerHistoryPoint> points, std::uint32_t now, std::uint32_t span, std::size_t slots);
