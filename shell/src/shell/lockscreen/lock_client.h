#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class SessionBus;

namespace sdbus {
  class IProxy;
} // namespace sdbus

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

  [[nodiscard]] bool delegating() const noexcept { return m_seen; }
  [[nodiscard]] bool owned() const noexcept { return m_owned; }
  // Lock requested or held; stays true from lock() until the process reports unlocked or aborted.
  [[nodiscard]] bool active() const noexcept { return m_requested || m_active || m_locked; }
  [[nodiscard]] bool locked() const noexcept { return m_locked; }

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
  void applyState(bool active, bool locked);
  void call(const char* method);

  std::unique_ptr<sdbus::IProxy> m_daemon;
  std::unique_ptr<sdbus::IProxy> m_lock;
  bool m_resolved = false;
  bool m_owned = false;
  bool m_seen = false;
  bool m_requested = false;
  bool m_active = false;
  bool m_locked = false;
  std::string m_requestReason;
  std::vector<std::function<void()>> m_whenResolved;
  std::function<void(const std::string&)> m_stateCallback;
  std::function<void(const std::vector<std::int64_t>&)> m_failedAttemptsCallback;
  std::shared_ptr<int> m_alive = std::make_shared<int>(0);
};
