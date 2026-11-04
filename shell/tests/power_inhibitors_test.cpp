#include "shell/session/power_inhibitors.h"
#include "tests/test_check.h"

int main() {
  const std::vector<LogindInhibitor> list{
      {.what = "sleep:idle", .who = "player", .mode = "block", .pid = 10},
      {.what = "shutdown:sleep", .who = "shell", .mode = "block", .pid = 99},
      {.what = "shutdown", .who = "updater", .mode = "delay", .pid = 11},
      {.what = "handle-power-key", .who = "daemon", .mode = "block", .pid = 12},
  };
  // Block locks of other processes count; delay locks and the shell's own locks never do.
  TEST_CHECK(powerActionBlocked(list, "sleep", 99));
  TEST_CHECK(!powerActionBlocked(list, "shutdown", 99));
  TEST_CHECK(powerActionBlocked(list, "shutdown", 1));
  TEST_CHECK(!powerActionBlocked({}, "shutdown", 1));
  return 0;
}
