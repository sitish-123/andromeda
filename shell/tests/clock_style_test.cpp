#include "tests/test_check.h"
#include "ui/auth/clock_style.h"

#include <ctime>
#include <string>
#include <vector>

namespace {

  std::tm at(int h, int m, int s) {
    std::tm t{};
    t.tm_hour = h;
    t.tm_min = m;
    t.tm_sec = s;
    return t;
  }

  using Lines = std::vector<std::string>;

} // namespace

int main() {
  const ClockStyle& line = clockStyleById("classic");
  TEST_CHECK((clockLines(line, at(9, 5, 7), {.h24 = true}) == Lines{"09:05"}));
  TEST_CHECK((clockLines(line, at(21, 5, 7), {.h24 = false}) == Lines{"9:05"}));
  TEST_CHECK((clockLines(line, at(0, 30, 0), {.h24 = false}) == Lines{"12:30"}));
  TEST_CHECK((clockLines(line, at(21, 5, 7), {.h24 = true, .seconds = true}) == Lines{"21:05:07"}));

  const ClockStyle& stacked = clockStyleById("stacked");
  TEST_CHECK((clockLines(stacked, at(10, 42, 3), {}) == Lines{"10", "42"}));
  TEST_CHECK((clockLines(stacked, at(10, 42, 3), {.seconds = true}) == Lines{"10", "42", "03"}));

  const ClockStyle& words = clockStyleById("words");
  TEST_CHECK((clockLines(words, at(22, 42, 0), {}) == Lines{"ten", "forty-two"}));
  TEST_CHECK((clockLines(words, at(0, 0, 0), {}) == Lines{"twelve", "o'clock"}));
  TEST_CHECK((clockLines(words, at(7, 5, 0), {}) == Lines{"seven", "oh five"}));
  TEST_CHECK((clockLines(words, at(13, 15, 0), {}) == Lines{"one", "fifteen"}));

  TEST_CHECK(clockStyleById("no-such-style").id == clockStyles().front().id);
  TEST_CHECK(clockSizeFor("xl") == 120.0F && clockSizeFor("") == 0.0F);
  return 0;
}
