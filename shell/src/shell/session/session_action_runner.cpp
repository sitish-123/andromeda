#include "shell/session/session_action_runner.h"
#include "util/thread_name.h"

#include "compositors/compositor_detect.h"
#include "compositors/compositor_platform.h"
#include "core/deferred_call.h"
#include "core/log.h"
#include "core/process/process.h"
#include "i18n/i18n.h"
#include "notification/notifications.h"
#include "shell/lockscreen/lock_screen.h"
#include "util/string_utils.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <optional>
#include <sdbus-c++/sdbus-c++.h>
#include <string>
#include <string_view>
#include <system_error>
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
  constexpr auto kLoginInterface = "org.andromeda.System1.Login";

  std::unique_ptr<sdbus::IProxy> loginProxy(sdbus::IConnection& connection) {
    return sdbus::createProxy(
        connection, sdbus::ServiceName{"org.andromeda.System1"}, sdbus::ObjectPath{"/org/andromeda/System1/Login"}
    );
  }

  // `power` is a System1 Login action: reboot, power-off, suspend, hibernate or suspend-then-hibernate.
  [[nodiscard]] bool callPower(std::string_view action, const char* power, bool skipInhibitors = false) {
    try {
      const auto connection = sdbus::createSystemBusConnection();
      loginProxy(*connection)
          ->callMethod("Power")
          .onInterface(kLoginInterface)
          .withArguments(std::string(power), !skipInhibitors, skipInhibitors);
      kLog.info("{}: logind {} accepted", action, power);
      return true;
    } catch (const sdbus::Error& e) {
      kLog.warn("{}: logind {} failed: {}", action, power, e.what());
      return false;
    }
  }

  // "yes", "no", "challenge" (needs polkit), or "na" (not available on this hardware, e.g. no swap).
  [[nodiscard]] bool canPower(std::string_view action, const char* power) {
    try {
      const auto connection = sdbus::createSystemBusConnection();
      std::string reply;
      loginProxy(*connection)
          ->callMethod("CanPower")
          .onInterface(kLoginInterface)
          .withArguments(std::string(power))
          .storeResultsTo(reply);
      return reply == "yes";
    } catch (const sdbus::Error& e) {
      kLog.warn("{}: logind can {} failed: {}", action, power, e.what());
      return false;
    }
  }

  [[nodiscard]] bool setRebootToFirmwareSetup() {
    try {
      const auto connection = sdbus::createSystemBusConnection();
      loginProxy(*connection)
          ->callMethod("SetRebootToFirmwareSetup")
          .onInterface(kLoginInterface)
          .withArguments(true, false);
      return true;
    } catch (const sdbus::Error& e) {
      kLog.warn("reboot_uefi: logind SetRebootToFirmwareSetup failed: {}", e.what());
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
      std::string_view action, const std::optional<std::string>& commandOverride, const char* power,
      PowerLaunchMode mode, bool checkAvailable = false
  ) {
    if (commandOverride.has_value()) {
      return runPowerOverride(action, *commandOverride, mode);
    }
    if (checkAvailable && !canPower(action, power)) {
      kLog.warn("{}: logind reports {} unavailable", action, power);
      return false;
    }
    return callPower(action, power);
  }

  [[nodiscard]] bool rebootUefiSupported() { return canPower("reboot_uefi", "reboot-to-firmware-setup"); }

  [[nodiscard]] bool requestLock(LockScreen& lockScreen) {
    logActionContext("lock");
    if (!lockScreen.lock()) {
      kLog.warn("lock: lock screen request failed");
      return false;
    }
    kLog.info("lock: lock screen requested");
    return true;
  }

  // The hook runs through the async process manager on the main loop; the action runs after it
  // finishes, on a worker thread because it may block on D-Bus or a command.
  void runPowerAction(
      const std::weak_ptr<void>& guard, SessionActionHooks::AsyncHook hook, std::function<bool()> action,
      std::string_view actionName, std::function<void()> failed
  ) {
    auto runAction = [guard, action = std::move(action), actionName = std::string(actionName),
                      failed = std::move(failed)] {
      std::thread([guard, action, actionName, failed] {
        nameThisThread("dsk-action");
        if (!action()) {
          kLog.warn("{} failed after hooks completed", actionName);
          if (failed) {
            DeferredCall::callLater([guard, failed] {
              if (!guard.expired()) {
                failed();
              }
            });
          }
        }
      }).detach();
    };
    if (!hook) {
      runAction();
      return;
    }
    hook([guard, runAction = std::move(runAction), actionName = std::string(actionName)](bool ok) {
      if (guard.expired()) {
        return;
      }
      // A failing hook (e.g. killing an app that isn't running) must not block the action.
      if (!ok) {
        kLog.warn("{}: a configured hook failed, continuing", actionName);
      }
      runAction();
    });
  }

  void runShellCommand(
      const std::weak_ptr<void>& guard, SessionActionHooks::AsyncHook hook, std::string command,
      std::string_view actionName, std::function<void()> failed
  ) {
    auto launch = [guard, command = std::move(command), actionName = std::string(actionName), failed = std::move(failed)] {
      if (!failed) {
        if (!process::runAsync(command)) {
          kLog.warn("{}: command failed", actionName);
        }
        return;
      }
      // A caller that waits on the outcome needs the exit status: tracked, not detached.
      const bool started = process::runAsync(
          command, process::RunCallbacks{.onExit = [guard, actionName, failed](const process::RunResult& result) {
            if (!result && !guard.expired()) {
              kLog.warn("{}: command exited with {}", actionName, result.exitCode);
              failed();
            }
          }}
      );
      if (!started) {
        kLog.warn("{}: command failed", actionName);
        failed();
      }
    };
    if (!hook) {
      launch();
      return;
    }
    hook([guard, launch = std::move(launch), actionName = std::string(actionName)](bool ok) {
      if (guard.expired()) {
        return;
      }
      // A failing hook (e.g. killing an app that isn't running) must not block the action.
      if (!ok) {
        kLog.warn("{}: a configured hook failed, continuing", actionName);
      }
      launch();
    });
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
  m_hibernateCommandOverride = power.hibernate;
  m_suspendThenHibernateCommandOverride = power.suspendThenHibernate;
}

void SessionActionRunner::invoke(const SessionPanelActionConfig& cfg, std::function<void()> failed) const {
  if (cfg.command.has_value() && !StringUtils::trim(*cfg.command).empty()) {
    runShellCommand(m_aliveGuard, hookFor(cfg.action), StringUtils::trim(*cfg.command), cfg.action, failed);
    return;
  }

  if (cfg.action == "command") {
    kLog.warn("session panel: custom action missing command");
    return;
  }

  if (cfg.action == "logout") {
    runPowerAction(m_aliveGuard, m_hooks.onLogout, [platform = &m_platform] { return platform->requestSessionExit(); }, "logout", failed);
    return;
  }
  if (cfg.action == "suspend") {
    runPowerAction(m_aliveGuard, {}, [this] { return requestSuspendDetached(); }, "suspend", failed);
    return;
  }
  if (cfg.action == "lock_and_suspend") {
    if (!lockThenSuspendDetached()) {
      notify::error("Noctalia", i18n::tr("session.errors.lock-title"), i18n::tr("session.errors.lock-body"));
    }
    return;
  }
  // A caller that waits on the outcome gets an override's exit status (a detached launch only reports the spawn).
  const std::optional<std::string>& endOverride = cfg.action == "reboot" ? m_rebootCommandOverride
      : cfg.action == "shutdown"                                     ? m_shutdownCommandOverride
                                                                     : std::nullopt;
  if (failed && endOverride.has_value() && !StringUtils::trim(*endOverride).empty()) {
    runShellCommand(m_aliveGuard, hookFor(cfg.action), StringUtils::trim(*endOverride), cfg.action, failed);
    return;
  }
  if (cfg.action == "reboot") {
    runPowerAction(m_aliveGuard, m_hooks.onReboot, [this] { return requestRebootDetached(); }, "reboot", failed);
    return;
  }
  if (cfg.action == "reboot_uefi") {
    // Check before the rebooting hook runs so an unsupported system doesn't fire it for nothing.
    if (!rebootUefiSupported()) {
      kLog.error("reboot_uefi: requires systemd and a UEFI boot");
      if (failed) {
        failed();
      }
      return;
    }
    runPowerAction(m_aliveGuard, m_hooks.onReboot, [this] { return requestRebootUefiDetached(); }, "reboot_uefi", failed);
    return;
  }
  if (cfg.action == "shutdown") {
    runPowerAction(m_aliveGuard, m_hooks.onShutdown, [this] { return requestShutdownDetached(); }, "shutdown", failed);
    return;
  }
  if (cfg.action == "hibernate") {
    runPowerAction(m_aliveGuard, {}, [this]() { return requestHibernateDetached(); }, "hibernate", failed);
    return;
  }
  if (cfg.action == "suspend_then_hibernate") {
    runPowerAction(m_aliveGuard, {}, [this]() { return requestSuspendThenHibernateDetached(); }, "suspend_then_hibernate", failed);
    return;
  }
  if (cfg.action == "lock") {
    if (!lock()) {
      notify::error("Noctalia", i18n::tr("session.errors.lock-title"), i18n::tr("session.errors.lock-body"));
    }
    return;
  }
}

SessionActionHooks::AsyncHook SessionActionRunner::hookFor(std::string_view action) const {
  if (action == "logout") {
    return m_hooks.onLogout;
  }
  if (action == "reboot" || action == "reboot_uefi") {
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
      runPowerActionResolved("suspend", m_suspendCommandOverride, "suspend", PowerLaunchMode::Detached);
  if (!started && m_hooks.onPlainSuspendAborted) {
    m_hooks.onPlainSuspendAborted();
  }
  return started;
}

bool SessionActionRunner::requestRebootDetached() const {
  logActionContext("reboot");
  std::scoped_lock lock(m_powerMutex);
  return runPowerActionResolved("reboot", m_rebootCommandOverride, "reboot", PowerLaunchMode::Detached);
}

bool SessionActionRunner::requestRebootUefiDetached() const {
  logActionContext("reboot_uefi");
  std::scoped_lock lock(m_powerMutex);
  return setRebootToFirmwareSetup() && callPower("reboot_uefi", "reboot");
}

bool SessionActionRunner::requestPowerSkippingInhibitors(std::string_view action) const {
  const char* power = action == "suspend" ? "suspend"
      : action == "reboot"                ? "reboot"
      : action == "shutdown"              ? "power-off"
                                          : nullptr;
  if (power == nullptr) {
    return false;
  }
  logActionContext(action);
  std::scoped_lock lock(m_powerMutex);
  return callPower(action, power, true);
}

bool SessionActionRunner::requestShutdownDetached() const {
  logActionContext("shutdown");
  std::scoped_lock lock(m_powerMutex);
  return runPowerActionResolved("shutdown", m_shutdownCommandOverride, "power-off", PowerLaunchMode::Detached);
}

bool SessionActionRunner::requestHibernateDetached() const {
  logActionContext("hibernate");
  std::scoped_lock lock(m_powerMutex);
  return runPowerActionResolved(
      "hibernate", m_hibernateCommandOverride, "hibernate", PowerLaunchMode::Detached, true
  );
}

bool SessionActionRunner::requestSuspendThenHibernateDetached() const {
  logActionContext("suspend_then_hibernate");
  if (m_hooks.onBeforePlainSuspend) {
    m_hooks.onBeforePlainSuspend();
  }
  std::scoped_lock lock(m_powerMutex);
  const bool started = runPowerActionResolved(
      "suspend_then_hibernate", m_suspendThenHibernateCommandOverride, "suspend-then-hibernate",
      PowerLaunchMode::Detached, true
  );
  if (!started && m_hooks.onPlainSuspendAborted) {
    m_hooks.onPlainSuspendAborted();
  }
  return started;
}

bool SessionActionRunner::lockThenSuspendDetached() const {
  m_lockScreen.runAfterSessionLocked([this] { (void)requestSuspendDetached(); });
  m_lockScreen.prepareForSleep();
  return true;
}

bool SessionActionRunner::suspendBlocking() const {
  logActionContext("suspend");
  if (m_hooks.onBeforePlainSuspend) {
    m_hooks.onBeforePlainSuspend();
  }
  std::scoped_lock lock(m_powerMutex);
  const bool started =
      runPowerActionResolved("suspend", m_suspendCommandOverride, "suspend", PowerLaunchMode::Blocking);
  if (!started && m_hooks.onPlainSuspendAborted) {
    m_hooks.onPlainSuspendAborted();
  }
  return started;
}

bool SessionActionRunner::rebootBlocking() const {
  logActionContext("reboot");
  std::scoped_lock lock(m_powerMutex);
  return runPowerActionResolved("reboot", m_rebootCommandOverride, "reboot", PowerLaunchMode::Blocking);
}

bool SessionActionRunner::shutdownBlocking() const {
  logActionContext("shutdown");
  std::scoped_lock lock(m_powerMutex);
  return runPowerActionResolved("shutdown", m_shutdownCommandOverride, "power-off", PowerLaunchMode::Blocking);
}
