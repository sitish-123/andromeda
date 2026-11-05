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

  using State = ScreenActivityLog::State;
  const auto spans = log.spans(h, h + 5400);
  TEST_CHECK(spans.size() == 5);
  TEST_CHECK(spans[0].start == h + 3000 && spans[0].end == h + 4000 && spans[0].state == State::On);
  TEST_CHECK(spans[1].end == h + 4200 && spans[1].state == State::Idle);
  TEST_CHECK(spans[3].start == h + 4300 && spans[3].end == h + 5300 && spans[3].state == State::Off);
  TEST_CHECK(spans[4].end == h + 5400 && spans[4].state == State::On);

  // A suspend shows only in the clocks: the compositor books it as sleep, not as screen-off or screen-on time.
  ScreenActivityLog slept;
  slept.setScreenOn(true, h);
  slept.setScreenOn(false, h + 100);
  slept.suspended(1000, h + 2000);
  slept.setScreenOn(true, h + 2000);
  const auto withSleep = slept.spans(h, h + 2100);
  TEST_CHECK(withSleep.size() == 4);
  TEST_CHECK(withSleep[1].state == State::Off && withSleep[1].end == h + 1000);
  TEST_CHECK(withSleep[2].state == State::Sleep && withSleep[2].start == h + 1000 && withSleep[2].end == h + 2000);
  TEST_CHECK(slept.hours(h, h + 3600, h + 2100)[0].onSeconds == 200);

  ScreenActivityLog restored;
  restored.parse(log.serialize() + "garbage line\n");
  const auto again = restored.hours(h, h + 3600, h + 3600);
  TEST_CHECK(again.size() == 1 && again[0].onSeconds == 600);
  ScreenActivityLog restoredSleep;
  restoredSleep.parse(slept.snapshot(h + 2100));
  const auto back = restoredSleep.spans(h, h + 2100);
  TEST_CHECK(back.size() == 4 && back[2].state == State::Sleep && back[3].end == h + 2100);
  return 0;
}
