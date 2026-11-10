#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

class SystemBus;

namespace sdbus {
  class IProxy;
  class ObjectPath;
}

class LogindService {
public:
  using PrepareForSleepCallback = std::function<void(bool sleeping)>;
  using SessionLockCallback = std::function<void()>;

  explicit LogindService(SystemBus& bus);
  ~LogindService();

  void setPrepareForSleepCallback(PrepareForSleepCallback callback);
  void setLockCallback(SessionLockCallback callback);
  void setUnlockCallback(SessionLockCallback callback);

  void setSessionLockIntegrationEnabled(bool enabled);
  // Sleep-delay inhibit for lock-before-suspend. Released when session lock integration is off.
  void setLockBeforeSuspendEnabled(bool enabled);
  void setSessionLockedHint(bool locked);

  [[nodiscard]] bool supportsIdleInhibit() const noexcept;
  [[nodiscard]] bool hasIdleInhibit() const noexcept;
  bool acquireIdleInhibit();
  void releaseIdleInhibit();

  // Delay-type sleep inhibit: holds off suspend/hibernate until released so the
  // session can lock first (same pattern as swayidle/hypridle). Only Held delays a suspend.
  enum class SleepDelay : std::uint8_t { None, Pending, PendingCancelled, Held };
  [[nodiscard]] SleepDelay sleepDelay() const noexcept { return m_sleepDelay; }
  void acquireSleepDelayInhibit();
  void releaseSleepDelayInhibit();

private:
  void ensureSessionLockMonitor();
  void attachSessionProxy(const sdbus::ObjectPath& sessionPath);

  SystemBus& m_bus;
  std::unique_ptr<sdbus::IProxy> m_managerProxy;
  std::unique_ptr<sdbus::IProxy> m_sessionProxy;
  bool m_sessionLockIntegrationEnabled = false;
  bool m_sessionResolving = false;
  std::optional<bool> m_pendingLockedHint;
  std::shared_ptr<int> m_aliveToken = std::make_shared<int>(0);
  PrepareForSleepCallback m_prepareForSleepCallback;
  SessionLockCallback m_lockCallback;
  SessionLockCallback m_unlockCallback;
  int m_idleInhibitFd = -1;
  // Valid only in SleepDelay::Held.
  int m_sleepDelayInhibitFd = -1;
  SleepDelay m_sleepDelay = SleepDelay::None;
};
