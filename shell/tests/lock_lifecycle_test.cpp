#include "shell/lockscreen/lock_lifecycle.h"
#include "tests/test_check.h"

#include <algorithm>
#include <random>

namespace {

  using Phase = LockLifecycle::Phase;
  using Event = LockLifecycle::Event;
  using Wire = LockLifecycle::Wire;
  using Relock = LockLifecycle::Relock;

  // The compositor side of one ext_session_lock_v1 object, with wlroots' legality rules.
  struct Compositor {
    bool alive = false;
    bool lockedSent = false;
    bool finishedSent = false;

    void check(Wire wire) const {
      if (wire == Wire::Destroy) {
        TEST_CHECK(alive && finishedSent);
      } else if (wire == Wire::UnlockAndDestroy) {
        TEST_CHECK(alive && (lockedSent || finishedSent));
      }
    }
  };

  void testRandomSequencesKeepProtocolLegal() {
    std::mt19937 rng(0x10cc);
    for (int run = 0; run < 2000; ++run) {
      LockLifecycle machine;
      Compositor compositor;
      for (int step = 0; step < 40; ++step) {
        const bool wasActive = machine.active();
        const auto pick = std::uniform_int_distribution<int>(0, 9)(rng);
        LockLifecycle::Step result;
        if (pick == 8) {
          const auto kind = static_cast<Relock>(std::uniform_int_distribution<int>(1, 3)(rng));
          const auto before = machine.queuedRelock();
          (void)machine.queueRelock(kind);
          // A queued relock never loses strength: a phone or sleep request keeps its no-grace promise.
          if (machine.tearingDown()) {
            TEST_CHECK(machine.queuedRelock() == std::max(before, kind));
          }
          continue;
        }
        if (pick == 9) {
          continue;
        }
        const auto event = static_cast<Event>(pick);
        if (event == Event::Locked && (!compositor.alive || compositor.lockedSent || compositor.finishedSent)) {
          continue;
        }
        if (event == Event::Finished && (!compositor.alive || compositor.finishedSent)) {
          continue;
        }
        if (event == Event::Locked) {
          compositor.lockedSent = true;
        } else if (event == Event::Finished) {
          compositor.finishedSent = true;
        }
        const auto queued = machine.queuedRelock();
        result = machine.on(event);
        if (result.accepted && event == Event::Requested) {
          compositor = Compositor{.alive = true};
        }
        compositor.check(result.wire);
        if (wasActive && machine.phase() == Phase::Idle) {
          TEST_CHECK(result.wire != Wire::None);
          compositor = {};
          // The relock outlives the teardown hooks, so they still see a lock ahead (sleep delay, post-lock action).
          TEST_CHECK(machine.lockAhead() == (queued != Relock::None));
          TEST_CHECK(machine.takeRelock() == queued);
        }
        TEST_CHECK(machine.active() == compositor.alive);
        TEST_CHECK(!machine.locked() || compositor.lockedSent);
        TEST_CHECK(machine.phase() != Phase::Idle || machine.queuedRelock() == Relock::None);
        // A teardown without a queued relock reports no lock ahead, so a lock request queues one.
        if (machine.phase() == Phase::Unlocking || machine.phase() == Phase::Cancelling) {
          TEST_CHECK(machine.lockAhead() == (machine.queuedRelock() != Relock::None));
        }
        if (event == Event::Locked || event == Event::Finished) {
          TEST_CHECK(result.accepted);
        }
      }
      // The owner dies in any phase: no request goes out, so a locked session stays locked.
      TEST_CHECK(machine.on(Event::Abandon).wire == Wire::None);
      TEST_CHECK(machine.phase() == Phase::Idle && !machine.lockAhead());
    }
  }

  void testUnlockWhilePendingWaitsForTheAnswer() {
    LockLifecycle machine;
    (void)machine.on(Event::Start);
    (void)machine.on(Event::Requested);
    const auto unlock = machine.on(Event::Unlock);
    TEST_CHECK(unlock.accepted && unlock.wire == Wire::None);
    TEST_CHECK(machine.phase() == Phase::Cancelling);
    TEST_CHECK(machine.on(Event::Locked).wire == Wire::UnlockAndDestroy);
    TEST_CHECK(machine.phase() == Phase::Idle);
  }

  void testRelockQueuedDuringUnlockSurvivesFinished() {
    LockLifecycle machine;
    (void)machine.on(Event::Start);
    (void)machine.on(Event::Requested);
    (void)machine.on(Event::Locked);
    (void)machine.on(Event::Unlock);
    TEST_CHECK(!machine.lockAhead());
    TEST_CHECK(machine.queueRelock(Relock::Phone));
    TEST_CHECK(machine.lockAhead());
    (void)machine.on(Event::Finished);
    TEST_CHECK(machine.lockAhead());
    TEST_CHECK(machine.takeRelock() == Relock::Phone);
    TEST_CHECK(machine.queuedRelock() == Relock::None && !machine.lockAhead());
  }

  // logind Lock during the unlock animation queues Plain; a later phone lock (password only) or sleep lock (no
  // grace) must replace it, and a later Plain must not weaken them.
  void testStrongerRelockReplacesQueuedPlain() {
    LockLifecycle machine;
    (void)machine.on(Event::Start);
    (void)machine.on(Event::Requested);
    (void)machine.on(Event::Locked);
    (void)machine.on(Event::Unlock);
    TEST_CHECK(machine.queueRelock(Relock::Plain));
    TEST_CHECK(machine.queueRelock(Relock::Sleep));
    TEST_CHECK(machine.queuedRelock() == Relock::Sleep);
    TEST_CHECK(machine.queueRelock(Relock::Phone));
    TEST_CHECK(machine.queueRelock(Relock::Sleep));
    TEST_CHECK(machine.queueRelock(Relock::Plain));
    TEST_CHECK(machine.queuedRelock() == Relock::Phone);
  }

  void testAbandonWhileLockedNeverUnlocks() {
    LockLifecycle machine;
    (void)machine.on(Event::Start);
    (void)machine.on(Event::Requested);
    (void)machine.on(Event::Locked);
    TEST_CHECK(machine.on(Event::Abandon).wire == Wire::None);
  }

} // namespace

int main() {
  testRandomSequencesKeepProtocolLegal();
  testUnlockWhilePendingWaitsForTheAnswer();
  testRelockQueuedDuringUnlockSurvivesFinished();
  testStrongerRelockReplacesQueuedPlain();
  testAbandonWhileLockedNeverUnlocks();
  return 0;
}
