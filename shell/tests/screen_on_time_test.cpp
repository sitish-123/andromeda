// Screen-on time since unplugged / since full charge: the period starts at the unplug after the last full charge,
// state spans sum per state, and drain per hour comes only from history segments spent mostly screen-on or off.

#include "system/screen_on_time.h"
#include "tests/test_check.h"

#include <cmath>
#include <vector>

int main() {
  using State = ScreenActivityLog::State;
  // Newest first, as UPower sends it: charged to full, unplugged at 1000, an hour screen-on, then two hours off.
  std::vector<UPowerHistoryPoint> points = {
      {.time = 11800, .value = 88.0, .state = BatteryState::Discharging},
      {.time = 4600, .value = 90.0, .state = BatteryState::Discharging},
      {.time = 1000, .value = 100.0, .state = BatteryState::Discharging},
      {.time = 100, .value = 100.0, .state = BatteryState::FullyCharged},
      {.time = 0, .value = 90.0, .state = BatteryState::Charging},
  };
  const std::vector<ScreenActivityLog::Span> spans = {
      {.start = 1000, .end = 4600, .state = State::On},
      {.start = 4600, .end = 8000, .state = State::Off},
      {.start = 8000, .end = 11800, .state = State::Sleep},
      {.start = 11800, .end = 12000, .state = State::On},
  };
  TEST_CHECK(sotPeriodStart(points, SotPeriod::SinceUnplugged) == 1000);
  TEST_CHECK(sotPeriodStart(points, SotPeriod::SinceFullCharge) == 1000);

  const ScreenOnTime sot = screenOnTime(spans, points, 1000, 12000);
  TEST_CHECK(sot.onSeconds == 3800 && sot.offSeconds == 3400 && sot.sleepSeconds == 3800 && sot.idleSeconds == 0);
  TEST_CHECK(std::abs(sot.usedPercent - 12.0) < 1e-9);
  TEST_CHECK(std::abs(sot.screenOnDrainPerHour - 10.0) < 1e-9);
  TEST_CHECK(std::abs(sot.screenOffDrainPerHour - 1.0) < 1e-9);

  // Plugged in again: no "since unplugged" period, but the full-charge one still stands.
  points.insert(points.begin(), {.time = 12000, .value = 88.0, .state = BatteryState::Charging});
  TEST_CHECK(!sotPeriodStart(points, SotPeriod::SinceUnplugged).has_value());
  TEST_CHECK(sotPeriodStart(points, SotPeriod::SinceFullCharge) == 1000);
  return 0;
}
