#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

class SessionBus;

namespace sdbus {
  class IProxy;
} // namespace sdbus

// What the shell knows of the lock process: whether it serves the bus name, and the lock it holds or was asked for.
class LockClientState {
public:
  // Reading: the process serves the name and its first state read is pending. Gone: it served the name and left.
  enum class Presence : std::uint8_t { Unresolved, Absent, Reading, Present, Gone };
  // Active: the process shows its lock surfaces but has not locked. While Locked, the process may be away or restarting.
  enum class Phase : std::uint8_t { Unlocked, Requested, Active, Locked, LockedRequested };
  enum class Notice : std::uint8_t { None, Locked, Unlocked, Aborted };

  struct OwnerChanged {
    bool owned = false;
  };
  // The GetAll reply; `ok` is false on a D-Bus error.
  struct StateRead {
    bool ok = false;
    bool active = false;
    bool locked = false;
  };
  struct StateChanged {
    std::string state;
  };
  struct LockAsked {
    std::string reason;
  };
  struct LockRefused {};
  using Event = std::variant<OwnerChanged, StateRead, StateChanged, LockAsked, LockRefused>;

  struct Result {
    bool accepted = false;
    Notice notice = Notice::None;
    // The first owner query has answered: run the whenResolved callbacks.
    bool resolved = false;
    bool readState = false;
    // Send Lock with this reason when the process serves the name.
    std::optional<std::string> sendLock;
  };

  [[nodiscard]] Presence presence() const noexcept { return m_presence; }
  [[nodiscard]] Phase phase() const noexcept { return m_phase; }
  [[nodiscard]] bool owned() const noexcept { return m_presence == Presence::Reading || m_presence == Presence::Present; }
  [[nodiscard]] bool seen() const noexcept { return owned() || m_presence == Presence::Gone; }
  [[nodiscard]] bool resolved() const noexcept {
    return m_presence != Presence::Unresolved && m_presence != Presence::Reading;
  }
  [[nodiscard]] bool active() const noexcept { return m_phase != Phase::Unlocked; }
  [[nodiscard]] bool locked() const noexcept { return m_phase == Phase::Locked || m_phase == Phase::LockedRequested; }
  [[nodiscard]] bool requested() const noexcept {
    return m_phase == Phase::Requested || m_phase == Phase::LockedRequested;
  }
  [[nodiscard]] const std::string& requestReason() const noexcept { return m_requestReason; }

  Result apply(Event event) {
    return std::visit([this](auto&& e) { return on(std::forward<decltype(e)>(e)); }, std::move(event));
  }

private:
  Result on(OwnerChanged event) {
    const bool wasResolved = resolved();
    if (event.owned) {
      m_presence = m_presence == Presence::Unresolved || m_presence == Presence::Reading ? Presence::Reading
                                                                                         : Presence::Present;
    } else if (m_presence == Presence::Unresolved) {
      m_presence = Presence::Absent;
    } else if (seen()) {
      m_presence = Presence::Gone;
    }
    Result result = accept(Notice::None);
    result.resolved = !wasResolved && resolved();
    result.readState = event.owned;
    return result;
  }

  Result on(StateRead event) {
    if (!seen()) {
      return {};
    }
    const bool wasResolved = resolved();
    if (m_presence == Presence::Reading) {
      m_presence = Presence::Present;
    }
    Result result = accept(Notice::None);
    result.resolved = !wasResolved;
    if (!event.ok) {
      return result;
    }
    // Only StateChanged reports an unlock: a restarted process reads unlocked until its relock lands.
    if (event.locked) {
      result.notice = settle(Phase::Locked);
    } else if (m_phase == Phase::Unlocked || m_phase == Phase::Active) {
      m_phase = event.active ? Phase::Active : Phase::Unlocked;
    }
    // A lock asked for while the process was away is sent now.
    if (requested() && !event.active) {
      result.sendLock = m_requestReason;
    }
    return result;
  }

  Result on(StateChanged event) {
    if (event.state == "locked") {
      return accept(settle(Phase::Locked));
    }
    if (event.state == "unlocked" || !locked()) {
      return accept(settle(Phase::Unlocked));
    }
    // A failed relock leaves the compositor's blank in place: the session is still locked.
    m_phase = Phase::Locked;
    return accept(Notice::None);
  }

  Result on(LockAsked event) {
    // A queued phone lock keeps its reason: it offers no passwordless grace.
    if (!requested() || event.reason == "phone") {
      m_requestReason = event.reason;
    }
    m_phase = locked() ? Phase::LockedRequested : Phase::Requested;
    Result result = accept(Notice::None);
    result.sendLock = std::move(event.reason);
    return result;
  }

  Result on(LockRefused /*event*/) {
    if (locked()) {
      return {};
    }
    return accept(settle(Phase::Unlocked));
  }

  static Result accept(Notice notice) {
    Result result;
    result.accepted = true;
    result.notice = notice;
    return result;
  }

  // The process reported locked or unlocked/aborted; either ends a pending request.
  Notice settle(Phase to) {
    const Phase from = std::exchange(m_phase, to);
    const bool wasLocked = from == Phase::Locked || from == Phase::LockedRequested;
    if (to == Phase::Locked) {
      return wasLocked ? Notice::None : Notice::Locked;
    }
    if (wasLocked) {
      return Notice::Unlocked;
    }
    return from != Phase::Unlocked ? Notice::Aborted : Notice::None;
  }

  Presence m_presence = Presence::Unresolved;
  Phase m_phase = Phase::Unlocked;
  std::string m_requestReason;
};

// Client of the lock process (`noctalia lock`, org.andromeda.Lock1). Once the shell has seen the process in this
// session it delegates to it while it serves the name or held the lock when it left: the restarted process locks
// itself again from the compositor's relock event. A session without the process (other compositors, no unit) never
// delegates.
class LockClient {
public:
  explicit LockClient(SessionBus& bus);
  ~LockClient();

  LockClient(const LockClient&) = delete;
  LockClient& operator=(const LockClient&) = delete;

  [[nodiscard]] bool delegating() const noexcept { return m_state.seen(); }
  [[nodiscard]] bool owned() const noexcept { return m_state.owned(); }
  // Lock requested or held; stays true from lock() until the process reports unlocked or aborted.
  [[nodiscard]] bool active() const noexcept { return m_state.active(); }
  [[nodiscard]] bool locked() const noexcept { return m_state.locked(); }

  // Runs fn once the first name-owner query has answered (at once when it has).
  void whenResolved(std::function<void()> fn);

  void lock(const std::string& reason);
  void primeCapture();
  void clearCapture();
  void prepareOutputPowerOff(std::function<void(bool)> done);
  void cancelOutputPowerOff();
  void outputsWoke();
  void skipLockOnNextSleep(bool skip);

  // "locked", "unlocked" or "aborted", also for the state found when the process appears.
  void setStateCallback(std::function<void(const std::string&)> callback) { m_stateCallback = std::move(callback); }
  void setFailedAttemptsCallback(std::function<void(const std::vector<std::int64_t>&)> callback) {
    m_failedAttemptsCallback = std::move(callback);
  }

private:
  void onOwner(bool owned);
  void readState();
  void apply(LockClientState::Event event);
  void sendLock(const std::string& reason);
  void call(const char* method);

  std::unique_ptr<sdbus::IProxy> m_daemon;
  std::unique_ptr<sdbus::IProxy> m_lock;
  LockClientState m_state;
  std::vector<std::function<void()>> m_whenResolved;
  std::function<void(const std::string&)> m_stateCallback;
  std::function<void(const std::vector<std::int64_t>&)> m_failedAttemptsCallback;
  std::shared_ptr<int> m_alive = std::make_shared<int>(0);
};
