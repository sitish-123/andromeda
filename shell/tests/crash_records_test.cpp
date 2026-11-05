// Settings > Problems lists only crashes of processes the desktop started, newest first.

#include "shell/settings/settings_content_problems.h"
#include "tests/test_check.h"

#include <string>

namespace {

  std::string dump(const char* exe, const char* pid, const char* timeUs, const char* unit) {
    return std::string(R"({"COREDUMP_EXE":")") + exe + R"(","COREDUMP_PID":")" + pid + R"(","COREDUMP_TIMESTAMP":")"
        + timeUs + R"(","COREDUMP_USER_UNIT":")" + unit + "\"}\n";
  }

} // namespace

int main() {
  const std::string journal = dump("/usr/bin/andromeda", "10", "1000", "andromeda.service")
      + dump("/home/u/src/desktop/compositor/build-debug/andromeda", "11", "4000", "foot-server.service")
      + dump("/usr/bin/andromeda", "12", "5000", "run-p22-i64.scope")
      + dump("/usr/lib/firefox/firefox", "13", "3000", "app-andromeda-1848-5.scope")
      + dump("/usr/bin/noctalia", "14", "2000", "noctalia.service");

  const auto records = settings::parseCrashRecords(journal);
  TEST_CHECK(records.size() == 3);
  TEST_CHECK(records[0].pid == 13);
  TEST_CHECK(records[1].pid == 14);
  TEST_CHECK(records[2].pid == 10);
  TEST_CHECK(records[0].exe == "/usr/lib/firefox/firefox");
  TEST_CHECK(records[0].timeUs == 3000);

  TEST_CHECK(settings::parseCrashRecords("").empty());
  TEST_CHECK(settings::parseCrashRecords("not json\n").empty());
  return 0;
}
