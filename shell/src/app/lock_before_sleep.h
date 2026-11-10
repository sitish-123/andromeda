#pragma once

#include <cstdint>
#include <functional>
#include <utility>

// Lock-before-sleep for the shell and the lock app: the logind delay inhibit holds a suspend until the
// lock frames are on screen. The caller owns the lock and the inhibit; this class owns the phase.
class LockBeforeSleep {
public:
  enum class Phase : std::uint8_t { Awake, AwakeSkipNextLock, LockingForSleep, ReadyForSleep };

  struct Hooks {
    std::function<bool()> enabled;
    std::function<bool()> lockReadyForSleep;
    // Locked, or a lock on its way (a relock queued behind an unlock counts).
    std::function<bool()> lockHeldOrQueued;
    // The lock runs its exit transition or a cancelled request.
    std::function<bool()> lockTearingDown;
    // False when the lock refuses to start.
    std::function<bool()> lock;
    std::function<void()> prepareLockForSleep;
    std::function<void()> acquireDelay;
    std::function<void()> releaseDelay;
  };

  explicit LockBeforeSleep(Hooks hooks) : m_hooks(std::move(hooks)) {}

  [[nodiscard]] Phase phase() const noexcept { return m_phase; }

  // A suspend the session starts itself (plain Suspend, or already locked) skips the lock.
  // A skip asked for during an unlock belongs to the lock that goes away: the next sleep still locks.
  void setSkipNextLock(bool skip) {
    if (skip && m_hooks.lockTearingDown()) {
      return;
    }
    if (m_phase == Phase::Awake || m_phase == Phase::AwakeSkipNextLock) {
      m_phase = skip ? Phase::AwakeSkipNextLock : Phase::Awake;
    }
  }

  void onPrepareForSleep(bool sleeping) {
    if (!sleeping) {
      m_phase = Phase::Awake;
      if (m_hooks.enabled()) {
        m_hooks.acquireDelay();
      }
      return;
    }
    if (m_phase == Phase::AwakeSkipNextLock || !m_hooks.enabled() || m_hooks.lockReadyForSleep()) {
      ready();
      return;
    }
    // Set first: lock() and prepareLockForSleep() may settle the lock before they return.
    m_phase = Phase::LockingForSleep;
    if (!m_hooks.lockHeldOrQueued() && !m_hooks.lock()) {
      ready();
      return;
    }
    m_hooks.prepareLockForSleep();
    // A deferred lock (no outputs yet) never reaches SessionLocked; it must not hold the suspend.
    if (m_phase == Phase::LockingForSleep && !m_hooks.lockHeldOrQueued()) {
      ready();
    }
  }

  // The lock frames are on screen, or their deadline passed: the suspend may go on.
  void onLockSettled() {
    if (m_phase == Phase::LockingForSleep) {
      ready();
    }
  }

  // The lock ended (unlocked or aborted). A relock queued behind it keeps the suspend held until it settles.
  void onLockEnded() {
    if (m_phase == Phase::LockingForSleep && !m_hooks.lockHeldOrQueued()) {
      ready();
    }
  }

private:
  void ready() {
    m_phase = Phase::ReadyForSleep;
    m_hooks.releaseDelay();
  }

  Hooks m_hooks;
  Phase m_phase = Phase::Awake;
};
