// The battery level chart reads UPower history in any order, starts where data starts, and marks charging time.

#include "system/battery_history.h"
#include "tests/test_check.h"

#include <cmath>
#include <vector>

namespace {

  bool near(float a, float b) { return std::abs(a - b) < 0.001F; }

} // namespace

int main() {
  constexpr std::uint32_t now = 10000;
  constexpr std::uint32_t span = 1000;
  // Newest first, as UPower sends it; the first sample sits halfway through the range.
  const std::vector<UPowerHistoryPoint> points = {
      {.time = 9900, .value = 80.0, .state = BatteryState::Discharging},
      {.time = 9700, .value = 60.0, .state = BatteryState::Charging},
      {.time = 9500, .value = 50.0, .state = BatteryState::Discharging},
      {.time = 10500, .value = 1.0, .state = BatteryState::Charging},
  };
  const BatteryLevelSeries series = batteryLevelSeries(points, now, span, 10);
  TEST_CHECK(near(series.startFraction, 0.5F));
  TEST_CHECK(series.level.size() == 5);
  TEST_CHECK(near(series.level.front(), 0.5F));
  TEST_CHECK(near(series.level[2], 0.6F));
  TEST_CHECK(near(series.level.back(), 0.8F));
  TEST_CHECK(series.charging.size() == 1);
  TEST_CHECK(near(series.charging[0].first, 0.7F) && near(series.charging[0].second, 0.9F));

  // A sample before the range: the line starts at the left edge with that value.
  const std::vector<UPowerHistoryPoint> older = {{.time = 8000, .value = 40.0, .state = BatteryState::Charging}};
  const BatteryLevelSeries carried = batteryLevelSeries(older, now, span, 4);
  TEST_CHECK(near(carried.startFraction, 0.0F));
  TEST_CHECK(carried.level.size() == 4 && near(carried.level[3], 0.4F));
  TEST_CHECK(carried.charging.size() == 1 && near(carried.charging[0].first, 0.0F) && near(carried.charging[0].second, 1.0F));

  TEST_CHECK(batteryLevelSeries({}, now, span, 4).level.empty());
  return 0;
}
