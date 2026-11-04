#include "system/battery_history.h"

#include <algorithm>

BatteryLevelSeries
batteryLevelSeries(std::span<const UPowerHistoryPoint> points, std::uint32_t now, std::uint32_t span, std::size_t slots) {
  BatteryLevelSeries out;
  if (span == 0 || slots == 0 || now < span) {
    return out;
  }
  std::vector<UPowerHistoryPoint> sorted(points.begin(), points.end());
  std::erase_if(sorted, [now](const UPowerHistoryPoint& p) { return p.time > now; });
  std::ranges::sort(sorted, {}, &UPowerHistoryPoint::time);
  if (sorted.empty()) {
    return out;
  }
  const std::uint32_t start = now - span;
  const auto fraction = [start, span](std::uint32_t t) {
    return std::clamp(static_cast<float>(static_cast<double>(t > start ? t - start : 0) / span), 0.0F, 1.0F);
  };

  const auto firstInRange = std::ranges::lower_bound(sorted, start, {}, &UPowerHistoryPoint::time);
  const bool knownAtStart = firstInRange != sorted.begin();
  const std::size_t firstSlot = knownAtStart || firstInRange == sorted.end()
      ? 0
      : std::min(slots - 1, static_cast<std::size_t>(fraction(firstInRange->time) * static_cast<float>(slots)));
  out.startFraction = static_cast<float>(firstSlot) / static_cast<float>(slots);

  std::size_t next = 0;
  float value = 0.0F;
  bool known = false;
  for (std::size_t slot = firstSlot; slot < slots; ++slot) {
    const double slotEnd = start + static_cast<double>(span) * static_cast<double>(slot + 1) / static_cast<double>(slots);
    while (next < sorted.size() && sorted[next].time <= slotEnd) {
      value = static_cast<float>(std::clamp(sorted[next].value / 100.0, 0.0, 1.0));
      known = true;
      ++next;
    }
    if (!known) {
      value = static_cast<float>(std::clamp(sorted.front().value / 100.0, 0.0, 1.0));
    }
    out.level.push_back(value);
  }

  for (std::size_t i = 0; i < sorted.size(); ++i) {
    if (sorted[i].state != BatteryState::Charging) {
      continue;
    }
    const float from = fraction(sorted[i].time);
    const float to = i + 1 < sorted.size() ? fraction(sorted[i + 1].time) : 1.0F;
    if (to <= from) {
      continue;
    }
    if (!out.charging.empty() && out.charging.back().second >= from) {
      out.charging.back().second = std::max(out.charging.back().second, to);
    } else {
      out.charging.emplace_back(from, to);
    }
  }
  return out;
}
