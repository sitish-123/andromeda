// A second update request waits for the run going on and starts when it ends; Done is never seen in between, so
// "Update and restart" restarts only after the update the user asked for.

#include "dbus/andromeda/update_run.h"
#include "tests/test_check.h"

#include <random>

namespace {

  const UpdateRequest kFull{};
  const UpdateRequest kAndromeda{.scope = UpdateScope::Andromeda};

  void checkInvariants(const UpdateRun& run) {
    TEST_CHECK(!run.queued().has_value() || run.state() == UpdateRunState::Running);
  }

} // namespace

int main() {
  {
    UpdateRun run;
    TEST_CHECK(run.request(kAndromeda) == kAndromeda);
    TEST_CHECK(!run.request(kFull).has_value());
    TEST_CHECK(run.queued() == kFull);
    TEST_CHECK(run.finished(true, false) == kFull);
    TEST_CHECK(run.state() == UpdateRunState::Running);
    TEST_CHECK(!run.finished(true, false).has_value());
    TEST_CHECK(run.state() == UpdateRunState::Done);
  }
  {
    UpdateRun run;
    TEST_CHECK(run.request(kFull).has_value());
    TEST_CHECK(!run.request(kFull).has_value());
    TEST_CHECK(!run.queued().has_value());
  }
  {
    UpdateRun run;
    run.runSeen();
    TEST_CHECK(!run.request(kFull).has_value());
    TEST_CHECK(run.finished(false, false) == kFull);
  }
  {
    UpdateRun run;
    TEST_CHECK(run.request(kAndromeda).has_value());
    TEST_CHECK(!run.request(kFull).has_value());
    TEST_CHECK(!run.finished(false, true).has_value());
    TEST_CHECK(run.state() == UpdateRunState::NeedsChoice);
    TEST_CHECK(!run.queued().has_value());
  }
  {
    UpdateRun run;
    TEST_CHECK(run.request(kAndromeda).has_value());
    TEST_CHECK(!run.request(kFull).has_value());
    run.stopped(true);
    TEST_CHECK(run.state() == UpdateRunState::Idle && !run.queued().has_value());
  }

  std::mt19937 rng(7);
  for (int sequence = 0; sequence < 2000; ++sequence) {
    UpdateRun run;
    for (int step = 0; step < 40; ++step) {
      const bool runningBefore = run.state() == UpdateRunState::Running;
      const bool queuedBefore = run.queued().has_value();
      std::optional<UpdateRequest> start;
      switch (rng() % 6) {
      case 0:
        start = run.request(rng() % 2 == 0 ? kFull : kAndromeda);
        TEST_CHECK(!start.has_value() || !runningBefore);
        break;
      case 1:
        run.runSeen();
        break;
      case 2:
      case 3:
        if (runningBefore) {
          const bool questions = rng() % 4 == 0;
          start = run.finished(rng() % 2 == 0, questions);
          TEST_CHECK(start.has_value() == (queuedBefore && !questions));
          TEST_CHECK(run.state() != UpdateRunState::Done || !queuedBefore);
        }
        break;
      case 4:
        run.stopped(rng() % 2 == 0);
        break;
      default:
        run.dismiss();
        break;
      }
      if (start.has_value()) {
        TEST_CHECK(run.state() == UpdateRunState::Running);
      }
      checkInvariants(run);
    }
  }
  return 0;
}
