#include "shell/lockscreen/lock_fail_delay.h"
#include "tests/test_check.h"

#include <filesystem>
#include <unistd.h>

int main() {
  const auto path =
      std::filesystem::temp_directory_path() / ("lock_fail_delay_test." + std::to_string(::getpid())) / "lock-fails";
  std::filesystem::remove_all(path.parent_path());

  {
    LockFailDelay delay(path);
    for (int i = 0; i < 4; ++i) {
      delay.recordRejected(1000);
    }
    TEST_CHECK(delay.remainingMs(1000) == 0);
    delay.recordRejected(1000);
    TEST_CHECK(delay.remainingMs(1000) == 30'000);
    delay.recordRejected(1000);
    TEST_CHECK(delay.remainingMs(1000) == 60'000);
  }

  {
    // A shell restart keeps the count and the wait.
    LockFailDelay delay(path);
    TEST_CHECK(delay.failures() == 6);
    TEST_CHECK(delay.remainingMs(31'000) == 30'000);
    // A clock set back cannot stretch the wait past the current step, and the countdown keeps running after it.
    TEST_CHECK(delay.remainingMs(-1'000'000) == 60'000);
    TEST_CHECK(delay.remainingMs(-990'000) == 50'000);
    for (int i = 0; i < 10; ++i) {
      delay.recordRejected(0);
    }
    TEST_CHECK(delay.remainingMs(0) == 300'000);
    delay.reset();
    TEST_CHECK(delay.remainingMs(0) == 0);
  }

  TEST_CHECK(LockFailDelay(path).failures() == 0);
  std::filesystem::remove_all(path.parent_path());
  return 0;
}
