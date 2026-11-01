#pragma once

#include "config/config_types.h"

#include <functional>
#include <memory>
#include <mutex>
#include <string_view>

class CompositorPlatform;
class LockScreen;

struct SessionActionHooks {
  using AsyncHook = std::function<void(std::function<void(bool ok)> done)>;
  AsyncHook onLogout;
  AsyncHook onReboot;
  AsyncHook onShutdown;
  // Armed before Noctalia-initiated suspend so PrepareForSleep can skip lock-before-sleep.
  std::function<void()> onBeforePlainSuspend;
  std::function<void()> onPlainSuspendAborted;
};

class SessionActionRunner {
public:
  explicit SessionActionRunner(CompositorPlatform& platform, LockScreen& lockScreen, SessionActionHooks hooks = {});

  void setHooks(SessionActionHooks hooks);
  void setPowerConfig(const ShellSessionConfig::ShellSessionPowerConfig& power);
  void invoke(const SessionPanelActionConfig& cfg) const;
  [[nodiscard]] bool lock() const;
  [[nodiscard]] bool requestSuspendDetached() const;
  [[nodiscard]] bool requestRebootDetached() const;
  [[nodiscard]] bool requestRebootUefiDetached() const;
  [[nodiscard]] bool requestShutdownDetached() const;
  [[nodiscard]] bool requestHibernateDetached() const;
  [[nodiscard]] bool requestSuspendThenHibernateDetached() const;
  [[nodiscard]] bool lockThenSuspendDetached() const;

private:
  [[nodiscard]] SessionActionHooks::AsyncHook hookFor(std::string_view action) const;
  [[nodiscard]] bool suspendBlocking() const;
  [[nodiscard]] bool rebootBlocking() const;
  [[nodiscard]] bool shutdownBlocking() const;

  CompositorPlatform& m_platform;
  LockScreen& m_lockScreen;
  SessionActionHooks m_hooks;
  std::shared_ptr<void> m_aliveGuard = std::make_shared<int>(0);

  // Session power command resolution is used from UI threads and worker threads
  // (panel/IPC/lock-and-suspend), so keep it internally synchronized.
  mutable std::mutex m_powerMutex;
  mutable std::optional<std::string> m_suspendCommandOverride;
  mutable std::optional<std::string> m_rebootCommandOverride;
  mutable std::optional<std::string> m_shutdownCommandOverride;
  mutable std::optional<std::string> m_hibernateCommandOverride;
  mutable std::optional<std::string> m_suspendThenHibernateCommandOverride;
};
