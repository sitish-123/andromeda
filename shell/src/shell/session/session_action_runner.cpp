#include "shell/session/session_action_runner.h"

#include "compositors/compositor_detect.h"
#include "compositors/compositor_platform.h"
#include "core/log.h"
#include "core/process/process.h"
#include "i18n/i18n.h"
#include "notification/notifications.h"
#include "shell/lockscreen/lock_screen.h"
#include "util/string_utils.h"

#include <chrono>
#include <cstdlib>
#include <functional>
#include <optional>
#include <sdbus-c++/sdbus-c++.h>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

  constexpr Logger kLog("session");
  constexpr std::chrono::milliseconds kPowerCommandTimeout{5000};

  enum class PowerLaunchMode {
    Blocking,
    Detached,
  };

  [[nodiscard]] const char* valueOrUnset(const char* value) {
    return value != nullptr && value[0] != '\0' ? value : "<unset>";
  }

  void logActionContext(std::string_view action) {
    const compositors::CompositorKind compositor = compositors::detect();
    kLog.info(
        "{} requested: compositor={} env_hint=\"{}\" xdg_session_id={} user={}", action, compositors::name(compositor),
        compositors::envHint(), valueOrUnset(std::getenv("XDG_SESSION_ID")), valueOrUnset(std::getenv("USER"))
    );
  }

  void
  logSessionCommandFailure(std::string_view action, std::string_view commandLabel, const process::RunResult& result) {
    if (result.timedOut) {
      kLog.warn("{}: {} timed out after {}ms", action, commandLabel, kPowerCommandTimeout.count());
    } else if (!result.err.empty()) {
      kLog.warn("{}: {} failed with code {}: {}", action, commandLabel, result.exitCode, result.err);
    } else if (!result.out.empty()) {
      kLog.warn("{}: {} failed with code {}: {}", action, commandLabel, result.exitCode, result.out);
    } else {
      kLog.warn("{}: {} failed with code {}", action, commandLabel, result.exitCode);
    }
  }

  // A connection of our own: the shell's system bus belongs to the main loop, and actions run on worker threads.
  [[nodiscard]] bool callLogind(std::string_view action, const char* method) {
    try {
      const auto connection = sdbus::createSystemBusConnection();
      const auto manager = sdbus::createProxy(
          *connection, sdbus::ServiceName{"org.freedesktop.login1"}, sdbus::ObjectPath{"/org/freedesktop/login1"}
      );
      manager->callMethod(method).onInterface("org.freedesktop.login1.Manager").withArguments(true);
      kLog.info("{}: logind {} accepted", action, method);
      return true;
    } catch (const sdbus::Error& e) {
      kLog.warn("{}: logind {} failed: {}", action, method, e.what());
      return false;
    }
  }

  [[nodiscard]] bool runPowerOverride(std::string_view action, const std::string& command, PowerLaunchMode mode) {
    if (mode == PowerLaunchMode::Detached) {
      if (process::runAsync(command)) {
        kLog.info("{}: custom override launched", action);
        return true;
      }
      kLog.warn("{}: custom override failed to launch", action);
      return false;
    }

    const process::RunResult result = process::runSync(command);
    if (result) {
      kLog.info("{}: custom override accepted", action);
      return true;
    }
    logSessionCommandFailure(action, command, result);
    return false;
  }

  [[nodiscard]] bool runPowerActionResolved(
      std::string_view action, const std::optional<std::string>& commandOverride, const char* logindMethod,
      PowerLaunchMode mode
  ) {
    if (commandOverride.has_value()) {
      return runPowerOverride(action, *commandOverride, mode);
    }
    return callLogind(action, logindMethod);
  }

  [[nodiscard]] bool requestLock(LockScreen& lockScreen) {
    logActionContext("lock");
    if (!lockScreen.lock()) {
      kLog.warn("lock: lock screen request failed");
      return false;
    }
    kLog.info("lock: lock screen requested");
    return true;
  }

  void runPowerAction(std::function<bool()> hook, std::function<bool()> action, std::string_view actionName) {
    std::thread([hook = std::move(hook), action = std::move(action), actionName = std::string(actionName)]() mutable {
      if (hook && !hook()) {
        kLog.warn("{} cancelled because a configured hook failed", actionName);
        return;
      }
      if (!action()) {
        kLog.warn("{} failed after hooks completed", actionName);
      }
    }).detach();
  }

  void runShellCommand(std::function<bool()> hook, std::string command, std::string_view actionName) {
    std::thread([hook = std::move(hook), command = std::move(command), actionName = std::string(actionName)]() mutable {
      if (hook && !hook()) {
        kLog.warn("{} cancelled because a configured hook failed", actionName);
        return;
      }
      if (!process::runAsync(command)) {
        kLog.warn("{}: command failed", actionName);
      }
    }).detach();
  }

} // namespace

SessionActionRunner::SessionActionRunner(CompositorPlatform& platform, LockScreen& lockScreen, SessionActionHooks hooks)
    : m_platform(platform), m_lockScreen(lockScreen), m_hooks(std::move(hooks)) {}

void SessionActionRunner::setHooks(SessionActionHooks hooks) { m_hooks = std::move(hooks); }

void SessionActionRunner::setPowerConfig(const ShellSessionConfig::ShellSessionPowerConfig& power) {
  std::scoped_lock lock(m_powerMutex);
  m_suspendCommandOverride = power.suspend;
  m_rebootCommandOverride = power.reboot;
  m_shutdownCommandOverride = power.shutdown;
}

void SessionActionRunner::invoke(const SessionPanelActionConfig& cfg) const {
  if (cfg.command.has_value() && !StringUtils::trim(*cfg.command).empty()) {
    runShellCommand(hookFor(cfg.action), StringUtils::trim(*cfg.command), cfg.action);
    return;
  }

  if (cfg.action == "command") {
    kLog.warn("session panel: custom action missing command");
    return;
  }

  if (cfg.action == "logout") {
    runPowerAction(m_hooks.onLogout, [platform = &m_platform]() { return platform->requestSessionExit(); }, "logout");
    return;
  }
  if (cfg.action == "suspend") {
    runPowerAction({}, [this]() { return requestSuspendDetached(); }, "suspend");
    return;
  }
  if (cfg.action == "lock_and_suspend") {
    if (!lockThenSuspendDetached()) {
      notify::error("Noctalia", i18n::tr("session.errors.lock-title"), i18n::tr("session.errors.lock-body"));
    }
    return;
  }
  if (cfg.action == "reboot") {
    runPowerAction(m_hooks.onReboot, [this]() { return requestRebootDetached(); }, "reboot");
    return;
  }
  if (cfg.action == "shutdown") {
    runPowerAction(m_hooks.onShutdown, [this]() { return requestShutdownDetached(); }, "shutdown");
    return;
  }
  if (cfg.action == "lock") {
    if (!lock()) {
      notify::error("Noctalia", i18n::tr("session.errors.lock-title"), i18n::tr("session.errors.lock-body"));
    }
    return;
  }
}

std::function<bool()> SessionActionRunner::hookFor(std::string_view action) const {
  if (action == "logout") {
    return m_hooks.onLogout;
  }
  if (action == "reboot") {
    return m_hooks.onReboot;
  }
  if (action == "shutdown") {
    return m_hooks.onShutdown;
  }
  return {};
}

bool SessionActionRunner::lock() const { return requestLock(m_lockScreen); }

bool SessionActionRunner::requestSuspendDetached() const {
  logActionContext("suspend");
  if (m_hooks.onBeforePlainSuspend) {
    m_hooks.onBeforePlainSuspend();
  }
  std::scoped_lock lock(m_powerMutex);
  const bool started =
      runPowerActionResolved("suspend", m_suspendCommandOverride, "Suspend", PowerLaunchMode::Detached);
  if (!started && m_hooks.onPlainSuspendAborted) {
    m_hooks.onPlainSuspendAborted();
  }
  return started;
}

bool SessionActionRunner::requestRebootDetached() const {
  logActionContext("reboot");
  std::scoped_lock lock(m_powerMutex);
  return runPowerActionResolved("reboot", m_rebootCommandOverride, "Reboot", PowerLaunchMode::Detached);
}

bool SessionActionRunner::requestShutdownDetached() const {
  logActionContext("shutdown");
  std::scoped_lock lock(m_powerMutex);
  return runPowerActionResolved("shutdown", m_shutdownCommandOverride, "PowerOff", PowerLaunchMode::Detached);
}

bool SessionActionRunner::lockThenSuspendDetached() const {
  m_lockScreen.runAfterSessionLocked([this]() { (void)requestSuspendDetached(); });
  return true;
}

bool SessionActionRunner::suspendBlocking() const {
  logActionContext("suspend");
  if (m_hooks.onBeforePlainSuspend) {
    m_hooks.onBeforePlainSuspend();
  }
  std::scoped_lock lock(m_powerMutex);
  const bool started =
      runPowerActionResolved("suspend", m_suspendCommandOverride, "Suspend", PowerLaunchMode::Blocking);
  if (!started && m_hooks.onPlainSuspendAborted) {
    m_hooks.onPlainSuspendAborted();
  }
  return started;
}

bool SessionActionRunner::rebootBlocking() const {
  logActionContext("reboot");
  std::scoped_lock lock(m_powerMutex);
  return runPowerActionResolved("reboot", m_rebootCommandOverride, "Reboot", PowerLaunchMode::Blocking);
}

bool SessionActionRunner::shutdownBlocking() const {
  logActionContext("shutdown");
  std::scoped_lock lock(m_powerMutex);
  return runPowerActionResolved("shutdown", m_shutdownCommandOverride, "PowerOff", PowerLaunchMode::Blocking);
}
