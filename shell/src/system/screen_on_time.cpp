#include "system/screen_on_time.h"

#include <algorithm>
#include <vector>

namespace {

  using State = ScreenActivityLog::State;

  std::vector<UPowerHistoryPoint> oldestFirst(std::span<const UPowerHistoryPoint> points) {
    std::vector<UPowerHistoryPoint> sorted(points.begin(), points.end());
    std::ranges::sort(sorted, {}, &UPowerHistoryPoint::time);
    return sorted;
  }

  bool onPower(BatteryState state) {
    return state == BatteryState::Charging || state == BatteryState::FullyCharged || state == BatteryState::PendingCharge;
  }

  // Full while on power; a discharging point at 100 % is the unplug after it, not the charge.
  bool full(const UPowerHistoryPoint& p) {
    return p.state == BatteryState::FullyCharged || (p.value >= 99.5 && p.state != BatteryState::Discharging);
  }

  // Seconds of [from, to) the spans spend with the screen on (on or idle), and in total.
  std::pair<std::int64_t, std::int64_t>
  screenOnWithin(std::span<const ScreenActivityLog::Span> spans, std::int64_t from, std::int64_t to) {
    std::int64_t on = 0;
    std::int64_t known = 0;
    for (const auto& span : spans) {
      const std::int64_t overlap = std::min(span.end, to) - std::max(span.start, from);
      if (overlap <= 0) {
        continue;
      }
      known += overlap;
      if (span.state == State::On || span.state == State::Idle) {
        on += overlap;
      }
    }
    return {on, known};
  }

} // namespace

std::optional<std::int64_t> sotPeriodStart(std::span<const UPowerHistoryPoint> points, SotPeriod period) {
  const std::vector<UPowerHistoryPoint> sorted = oldestFirst(points);
  if (sorted.empty()) {
    return std::nullopt;
  }
  if (period == SotPeriod::SinceUnplugged) {
    if (sorted.back().state != BatteryState::Discharging) {
      return std::nullopt;
    }
    for (std::size_t i = sorted.size() - 1; i > 0; --i) {
      if (onPower(sorted[i - 1].state)) {
        return sorted[i].time;
      }
    }
    return std::nullopt;
  }
  for (std::size_t i = sorted.size(); i > 0; --i) {
    if (!full(sorted[i - 1])) {
      continue;
    }
    for (std::size_t j = i; j < sorted.size(); ++j) {
      if (sorted[j].state == BatteryState::Discharging) {
        return sorted[j].time;
      }
    }
    return std::nullopt;
  }
  return std::nullopt;
}

ScreenOnTime screenOnTime(
    std::span<const ScreenActivityLog::Span> spans, std::span<const UPowerHistoryPoint> points, std::int64_t start,
    std::int64_t now
) {
  ScreenOnTime sot{.start = start};
  for (const auto& span : spans) {
    const std::int64_t overlap = std::min(span.end, now) - std::max(span.start, start);
    if (overlap <= 0) {
      continue;
    }
    switch (span.state) {
    case State::On:
      sot.onSeconds += overlap;
      break;
    case State::Idle:
      sot.idleSeconds += overlap;
      break;
    case State::Off:
      sot.offSeconds += overlap;
      break;
    case State::Sleep:
      sot.sleepSeconds += overlap;
      break;
    }
  }

  const std::vector<UPowerHistoryPoint> sorted = oldestFirst(points);
  double onDrop = 0.0;
  double offDrop = 0.0;
  std::int64_t onTime = 0;
  std::int64_t offTime = 0;
  for (std::size_t i = 1; i < sorted.size(); ++i) {
    const auto& a = sorted[i - 1];
    const auto& b = sorted[i];
    if (a.time < start || b.time > now || b.state != BatteryState::Discharging || b.value > a.value) {
      continue;
    }
    const double drop = a.value - b.value;
    sot.usedPercent += drop;
    const auto [on, known] = screenOnWithin(spans, a.time, b.time);
    const std::int64_t length = static_cast<std::int64_t>(b.time) - a.time;
    if (length <= 0 || known * 5 < length * 4) {
      continue;
    }
    if (on * 5 >= known * 4) {
      onDrop += drop;
      onTime += length;
    } else if (on * 5 <= known) {
      offDrop += drop;
      offTime += length;
    }
  }
  sot.screenOnDrainPerHour = onTime > 0 ? onDrop / (static_cast<double>(onTime) / 3600.0) : 0.0;
  sot.screenOffDrainPerHour = offTime > 0 ? offDrop / (static_cast<double>(offTime) / 3600.0) : 0.0;
  return sot;
}
