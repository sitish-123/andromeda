#include "app/lock_before_sleep.h"
#include "tests/test_check.h"

#include <cstdint>
#include <random>

using Phase = LockBeforeSleep::Phase;

namespace {

  enum class Lock : std::uint8_t { Unlocked, Locking, Ready, Unlocking };

  // Random logind, lock and config events against a fake lock and a fake delay inhibit.
  struct World {
    Lock lock = Lock::Unlocked;
    // A lock request during Unlocking queues a relock; LockScreen runs it after the unlock hooks.
    bool relockQueued = false;
    bool delayHeld = false;
    bool enabled = true;
    bool refuseLock = false;
    bool noOutputs = false;
    LockBeforeSleep machine{{
        .enabled = [this] { return enabled; },
        .lockReadyForSleep = [this] { return lock == Lock::Ready; },
        .lockHeldOrQueued = [this] { return lock == Lock::Locking || lock == Lock::Ready || relockQueued; },
        .lockTearingDown = [this] { return lock == Lock::Unlocking; },
        .lock =
            [this] {
              if (refuseLock) {
                return false;
              }
              if (lock == Lock::Unlocking) {
                relockQueued = true;
              } else if (!noOutputs) {
                lock = Lock::Locking;
              }
              return true;
            },
        .prepareLockForSleep = [] {},
        .acquireDelay = [this] { delayHeld = true; },
        .releaseDelay = [this] { delayHeld = false; },
    }};

    // LockScreen::endLock: the unlocked hook first, then the queued relock.
    void finishUnlock() {
      lock = Lock::Unlocked;
      machine.onLockEnded();
      if (!relockQueued) {
        return;
      }
      relockQueued = false;
      if (noOutputs) {
        machine.onLockSettled();
      } else {
        lock = Lock::Locking;
      }
    }
  };

  // lock_and_suspend during the unlock animation: the shell still sees the session locked and asks to skip.
  void testSkipDuringUnlockStillLocksBeforeSleep() {
    World w;
    w.machine.onPrepareForSleep(false);
    w.lock = Lock::Unlocking;
    w.machine.setSkipNextLock(true);
    w.finishUnlock();
    w.machine.onPrepareForSleep(true);
    TEST_CHECK(w.lock == Lock::Locking);
    TEST_CHECK(w.machine.phase() == Phase::LockingForSleep && w.delayHeld);
  }

} // namespace

int main() {
  testSkipDuringUnlockStillLocksBeforeSleep();
  std::mt19937 rng(0x5eed);
  for (int run = 0; run < 4000; ++run) {
    World w;
    for (int step = 0; step < 40; ++step) {
      const int event = static_cast<int>(rng() % 11);
      const Phase before = w.machine.phase();
      bool lockExpected = false;
      switch (event) {
      case 0: {
        const bool skip = w.machine.phase() == Phase::AwakeSkipNextLock;
        lockExpected = !skip && w.enabled && !w.refuseLock && !w.noOutputs;
        w.machine.onPrepareForSleep(true);
        TEST_CHECK(w.machine.phase() != Phase::Awake && w.machine.phase() != Phase::AwakeSkipNextLock);
        break;
      }
      case 1:
        w.machine.onPrepareForSleep(false);
        TEST_CHECK(w.machine.phase() == Phase::Awake);
        if (w.enabled) {
          TEST_CHECK(w.delayHeld);
        }
        break;
      case 2:
        if (w.lock == Lock::Locking) {
          w.lock = Lock::Ready;
          w.machine.onLockSettled();
        }
        break;
      case 3:
        if (w.lock == Lock::Locking || w.lock == Lock::Ready) {
          w.lock = Lock::Unlocked;
          w.machine.onLockEnded();
        }
        break;
      case 4: {
        const bool skip = (rng() & 1U) != 0;
        w.machine.setSkipNextLock(skip);
        if (skip && w.lock == Lock::Unlocking) {
          TEST_CHECK(w.machine.phase() == before);
        }
        break;
      }
      case 5:
        w.enabled = !w.enabled;
        break;
      case 6:
        w.refuseLock = !w.refuseLock;
        break;
      case 7:
        w.noOutputs = !w.noOutputs;
        break;
      case 8:
        if (w.lock == Lock::Locking || w.lock == Lock::Ready) {
          w.lock = Lock::Unlocking;
        }
        break;
      case 9:
        if (w.lock == Lock::Unlocking) {
          w.finishUnlock();
        }
        break;
      default:
        w.lock = w.lock == Lock::Unlocked ? Lock::Locking : w.lock;
        break;
      }
      if (lockExpected) {
        TEST_CHECK(w.lock == Lock::Unlocking ? w.relockQueued : w.lock != Lock::Unlocked);
      }
      // Only a lock still drawing its frames, or an unlock that a relock or its end settles, may hold the suspend.
      if (w.machine.phase() == Phase::LockingForSleep) {
        TEST_CHECK(w.lock == Lock::Locking || w.lock == Lock::Unlocking);
      }
      // The delay goes only when the lock is ready or none is on its way.
      if (event != 0 && before == Phase::LockingForSleep && w.machine.phase() == Phase::ReadyForSleep) {
        TEST_CHECK(w.lock == Lock::Ready || (w.lock != Lock::Locking && !w.relockQueued));
      }
      if (w.machine.phase() == Phase::ReadyForSleep) {
        TEST_CHECK(!w.delayHeld);
      }
    }
  }
  return 0;
}
