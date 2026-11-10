#pragma once

#include <cstdint>
#include <utility>

// The in-process ext_session_lock_v1 lifecycle. The compositor answers every lock request with exactly one of
// `locked` or `finished`; before that answer neither destroy nor unlock_and_destroy is legal (wlroots posts a
// protocol error for both), so an unlock that lands while Pending waits in Cancelling for the answer.
class LockLifecycle {
public:
  enum class Phase : std::uint8_t {
    Idle,
    Deferred,   // lock wanted, no usable output yet
    Starting,   // inside lock(), before the lock object exists
    Pending,    // lock object sent, compositor has not answered
    Cancelling, // unlock wanted while Pending: tear down on the answer
    Locked,
    Unlocking,  // exit transition running
  };
  enum class Event : std::uint8_t {
    Defer,
    Start,
    Requested,
    Locked,   // compositor `locked`
    Finished, // compositor `finished`
    Unlock,
    UnlockDone, // exit transition complete (or none)
    Fail,       // lock() could not finish setting up
    Abandon,    // the owner dies: drop the proxy only, so the compositor keeps the session locked
  };
  // Request to send on the lock object; None frees only the client proxy. Teardown to Idle follows it.
  enum class Wire : std::uint8_t { None, Destroy, UnlockAndDestroy };
  // Ordered by strength: Sleep offers no grace, Phone also asks for the password only.
  enum class Relock : std::uint8_t { None, Plain, Sleep, Phone };

  struct Step {
    bool accepted = false;
    Wire wire = Wire::None;
  };

  [[nodiscard]] Phase phase() const noexcept { return m_phase; }
  [[nodiscard]] Relock queuedRelock() const noexcept { return m_relock; }
  // The lock object exists and the compositor may hold the session locked.
  [[nodiscard]] bool active() const noexcept {
    return m_phase == Phase::Pending || m_phase == Phase::Cancelling || m_phase == Phase::Locked
        || m_phase == Phase::Unlocking;
  }
  // The compositor has confirmed the lock (locked event received).
  [[nodiscard]] bool locked() const noexcept { return m_phase == Phase::Locked || m_phase == Phase::Unlocking; }
  // The session is locked or a lock is on its way, including a relock queued behind a teardown. When false, a
  // lock request starts a lock (Idle, Deferred) or queues one (Unlocking, Cancelling).
  [[nodiscard]] bool lockAhead() const noexcept {
    return m_phase == Phase::Starting || m_phase == Phase::Pending || m_phase == Phase::Locked
        || m_relock != Relock::None;
  }

  // The exit transition or a cancelled request runs: the session goes unlocked unless a relock follows.
  [[nodiscard]] bool tearingDown() const noexcept { return m_phase == Phase::Unlocking || m_phase == Phase::Cancelling; }

  // The relock stays queued through the move to Idle until the owner takes it, after its teardown hooks ran.
  Relock takeRelock() noexcept { return std::exchange(m_relock, Relock::None); }

  // Queues a lock for after the current teardown; a stronger request replaces a weaker one. False when no
  // teardown is running.
  bool queueRelock(Relock relock) noexcept {
    if (!tearingDown()) {
      return false;
    }
    if (relock > m_relock) {
      m_relock = relock;
    }
    return true;
  }

  Step on(Event event) noexcept {
    using enum Phase;
    if (event == Event::Abandon) {
      m_relock = Relock::None;
      return idle(Wire::None);
    }
    switch (m_phase) {
    case Idle:
      if (event == Event::Defer) {
        return go(Deferred);
      }
      if (event == Event::Start) {
        return go(Starting);
      }
      break;
    case Deferred:
      if (event == Event::Defer) {
        return go(Deferred);
      }
      if (event == Event::Start) {
        return go(Starting);
      }
      if (event == Event::Unlock || event == Event::Fail) {
        return idle(Wire::None);
      }
      break;
    case Starting:
      if (event == Event::Defer) {
        return go(Deferred);
      }
      if (event == Event::Requested) {
        return go(Pending);
      }
      if (event == Event::Fail) {
        return idle(Wire::None);
      }
      break;
    case Pending:
      if (event == Event::Locked) {
        return go(Locked);
      }
      if (event == Event::Finished) {
        return idle(Wire::Destroy);
      }
      if (event == Event::Unlock || event == Event::Fail) {
        return go(Cancelling);
      }
      break;
    case Cancelling:
      if (event == Event::Locked) {
        return idle(Wire::UnlockAndDestroy);
      }
      if (event == Event::Finished) {
        return idle(Wire::Destroy);
      }
      break;
    case Locked:
      if (event == Event::Unlock) {
        return go(Unlocking);
      }
      if (event == Event::Finished) {
        return idle(Wire::UnlockAndDestroy);
      }
      break;
    case Unlocking:
      if (event == Event::UnlockDone || event == Event::Finished) {
        return idle(Wire::UnlockAndDestroy);
      }
      break;
    }
    return {};
  }

private:
  Step go(Phase next) noexcept {
    m_phase = next;
    return {.accepted = true};
  }
  Step idle(Wire wire) noexcept {
    m_phase = Phase::Idle;
    return {.accepted = true, .wire = wire};
  }

  Phase m_phase = Phase::Idle;
  Relock m_relock = Relock::None;
};
