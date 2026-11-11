#include "check.h"
#include "workspace/slide_mode.h"

#include <random>
#include <utility>

using namespace andromeda;

namespace {

  int holds = 0;
  int unmatchedReleases = 0;

  // Counts like qos::Controller's thaw holds; a release with none held is the bug the clamp used to hide.
  struct CountingHold {
    bool owns = true;
    CountingHold() { ++holds; }
    CountingHold(CountingHold&& other) noexcept : owns(std::exchange(other.owns, false)) {}
    CountingHold& operator=(CountingHold&&) = delete;
    ~CountingHold() {
      if (!owns) {
        return;
      }
      if (holds == 0) {
        ++unmatchedReleases;
      } else {
        --holds;
      }
    }
  };

  struct Data {
    int base = 0;
  };

  using Mode = SlideMode<Data, CountingHold>;

} // namespace

ANDROMEDA_TEST(aSecondSlideReplacesTheFirstWithoutLeakingOrDoubleReleasingItsHold) {
  holds = 0;
  unmatchedReleases = 0;
  {
    Mode mode;
    CHECK(mode.track({.base = 1}, CountingHold()));
    CHECK(mode.settle());
    CHECK(!mode.track({.base = 2}, CountingHold()));
    CHECK_EQ(holds, 1);
    CHECK(mode.finish().has_value());
    CHECK(mode.track({.base = 2}, CountingHold()));
    CHECK_EQ(mode.data()->base, 2);
    CHECK_EQ(holds, 1);
    CHECK(mode.finish().has_value());
    CHECK(!mode.finish().has_value());
  }
  CHECK_EQ(holds, 0);
  CHECK_EQ(unmatchedReleases, 0);
}

// Gestures, key switches, frames and teardown in any order, the way WorkspaceGroup drives the mode.
ANDROMEDA_TEST(randomSlideEventsHoldExactlyOneThawWhileASlideRuns) {
  enum class Phase { Idle, Tracking, Settling };
  for (unsigned seed = 1; seed <= 300; ++seed) {
    std::mt19937 rng(seed);
    holds = 0;
    unmatchedReleases = 0;
    {
      Mode mode;
      Phase model = Phase::Idle;
      int base = 0;
      for (int step = 0; step < 500; ++step) {
        switch (rng() % 6) {
          case 0: // slideBegin: a swipe starts, finishing whatever ran.
            mode.finish();
            CHECK(mode.track({.base = ++base}, CountingHold()));
            model = Phase::Tracking;
            break;
          case 1: // activate with animation: begin and settle at once.
            mode.finish();
            CHECK(mode.track({.base = ++base}, CountingHold()));
            CHECK(mode.settle());
            model = Phase::Settling;
            break;
          case 2: // slideSettle: a release, a carried overview offset, or a stale gesture end.
            CHECK_EQ(mode.settle(), model == Phase::Tracking);
            if (model == Phase::Tracking) {
              model = Phase::Settling;
            }
            break;
          case 3: // A frame ends the settle.
            if (mode.phase() == Mode::Phase::Settling) {
              mode.finish();
            }
            if (model == Phase::Settling) {
              model = Phase::Idle;
            }
            break;
          case 4: // A stray track while a slide runs is rejected.
            CHECK_EQ(mode.track({.base = -1}, CountingHold()), model == Phase::Idle);
            if (model == Phase::Idle) {
              model = Phase::Tracking;
            }
            break;
          default: // slideFinish from overview, reconcile, axis change, output loss.
            CHECK_EQ(mode.finish().has_value(), model != Phase::Idle);
            model = Phase::Idle;
            break;
        }
        CHECK_EQ(static_cast<int>(mode.phase()), static_cast<int>(model));
        CHECK_EQ(holds, model == Phase::Idle ? 0 : 1);
        CHECK_EQ(mode.data() != nullptr, model != Phase::Idle);
        CHECK_EQ(unmatchedReleases, 0);
      }
    }
    CHECK_EQ(holds, 0);
    CHECK_EQ(unmatchedReleases, 0);
  }
}

int main() { return RUN_TESTS(); }
