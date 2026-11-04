// Settings > Battery screen time: on vs idle per hour, short input pauses count as on, screen-off time counts as
// nothing, spans split at hour edges, and the log survives a restart through its text form.

#include "protocols/screen_activity_log.h"
#include "tests/test_check.h"

int main() {
  constexpr std::int64_t h = 3600 * 1000;
  ScreenActivityLog log;
  log.setScreenOn(true, h + 3000);   // 600 s on until the hour edge, then 300 s
  log.setUserIdle(true, h + 3900);   // 30 s pause: on
  log.setUserIdle(false, h + 3930);
  log.setUserIdle(true, h + 4000);   // 200 s pause: idle
  log.setUserIdle(false, h + 4200);
  log.setScreenOn(false, h + 4300);  // off for 1000 s: not counted
  log.setScreenOn(true, h + 5300);

  const auto hours = log.hours(h, h + 7200, h + 5400);
  TEST_CHECK(hours.size() == 2);
  TEST_CHECK(hours[0].start == h && hours[0].onSeconds == 600 && hours[0].idleSeconds == 0);
  TEST_CHECK(hours[1].onSeconds == 300 + 30 + 70 + 100 + 100);
  TEST_CHECK(hours[1].idleSeconds == 200);

  ScreenActivityLog restored;
  restored.parse(log.serialize() + "garbage line\n");
  const auto again = restored.hours(h, h + 3600, h + 3600);
  TEST_CHECK(again.size() == 1 && again[0].onSeconds == 600);
  return 0;
}
