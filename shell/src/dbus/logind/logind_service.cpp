#include "dbus/logind/logind_service.h"

#include "core/log.h"
#include "dbus/system_bus.h"

#include <cstdlib>
#include <fcntl.h>
#include <optional>
#include <sdbus-c++/Error.h>
#include <sdbus-c++/IProxy.h>
#include <sdbus-c++/Types.h>
#include <string>
#include <unistd.h>
#include <utility>

namespace {
  constexpr Logger kLog("logind");

  const sdbus::ServiceName kLogindBusName{"org.freedesktop.login1"};
  const sdbus::ObjectPath kLogindObjectPath{"/org/freedesktop/login1"};
  constexpr auto kLogindManagerInterface = "org.freedesktop.login1.Manager";
  constexpr auto kLogindSessionInterface = "org.freedesktop.login1.Session";

  // Inhibit locks stay alive until every duplicate of the fd is closed. Our detached
  // process launcher double-forks without closing fds, so without CLOEXEC `systemctl
  // suspend` inherits the sleep-delay inhibit and logind waits the full InhibitDelayMaxSec.
  [[nodiscard]] bool setCloexec(int fd) {
    if (fd < 0) {
      return false;
    }
    const int flags = ::fcntl(fd, F_GETFD);
    if (flags < 0) {
      return false;
    }
    return ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == 0;
  }
} // namespace

LogindService::LogindService(SystemBus& bus) : m_bus(bus) {
  m_managerProxy = sdbus::createProxy(m_bus.connection(), kLogindBusName, kLogindObjectPath);
  m_managerProxy->uponSignal("PrepareForSleep").onInterface(kLogindManagerInterface).call([this](bool sleeping) {
    if (m_prepareForSleepCallback) {
      m_prepareForSleepCallback(sleeping);
    }
  });
}

LogindService::~LogindService() {
  releaseSleepDelayInhibit();
  releaseIdleInhibit();
}

void LogindService::ensureSessionLockMonitor() {
  if (m_sessionProxy != nullptr || m_sessionResolving) {
    return;
  }
  m_sessionResolving = true;

  const std::weak_ptr<int> aliveToken = m_aliveToken;
  const auto byPid = [this, aliveToken] {
    m_managerProxy->callMethodAsync("GetSessionByPID")
        .onInterface(kLogindManagerInterface)
        .withArguments(static_cast<std::uint32_t>(::getpid()))
        .uponReplyInvoke([this, aliveToken](std::optional<sdbus::Error> e, sdbus::ObjectPath path) {
          if (aliveToken.expired()) {
            return;
          }
          m_sessionResolving = false;
          if (e.has_value()) {
            kLog.warn("failed to resolve logind session: {}", e->what());
            kLog.warn("logind session lock monitor disabled: session path unavailable");
            m_pendingLockedHint.reset();
            return;
          }
          attachSessionProxy(path);
        });
  };

  const char* sessionId = std::getenv("XDG_SESSION_ID");
  if (sessionId == nullptr || sessionId[0] == '\0') {
    byPid();
    return;
  }
  m_managerProxy->callMethodAsync("GetSession")
      .onInterface(kLogindManagerInterface)
      .withArguments(std::string(sessionId))
      .uponReplyInvoke([this, aliveToken, byPid, id = std::string(sessionId)](
                           std::optional<sdbus::Error> e, sdbus::ObjectPath path
                       ) {
        if (aliveToken.expired()) {
          return;
        }
        if (e.has_value()) {
          kLog.debug("failed to resolve logind session via XDG_SESSION_ID={}: {}", id, e->what());
          byPid();
          return;
        }
        m_sessionResolving = false;
        attachSessionProxy(path);
      });
}

void LogindService::attachSessionProxy(const sdbus::ObjectPath& sessionPath) {
  if (!m_sessionLockIntegrationEnabled) {
    return;
  }
  m_sessionProxy = sdbus::createProxy(m_bus.connection(), kLogindBusName, sessionPath);
  m_sessionProxy->uponSignal("Lock").onInterface(kLogindSessionInterface).call([this] {
    if (m_lockCallback) {
      m_lockCallback();
    }
  });
  m_sessionProxy->uponSignal("Unlock").onInterface(kLogindSessionInterface).call([this] {
    if (!m_bus.isNameOwner(kLogindBusName, m_sessionProxy->getCurrentlyProcessedMessage().getSender())) {
      kLog.warn("ignoring Unlock signal not sent by logind");
      return;
    }
    if (m_unlockCallback) {
      m_unlockCallback();
    }
  });
  kLog.info("logind session lock monitor active ({})", std::string(sessionPath.c_str()));
  if (m_pendingLockedHint.has_value()) {
    setSessionLockedHint(*std::exchange(m_pendingLockedHint, std::nullopt));
  }
}

void LogindService::setSessionLockIntegrationEnabled(bool enabled) {
  if (m_sessionLockIntegrationEnabled == enabled) {
    return;
  }
  m_sessionLockIntegrationEnabled = enabled;
  if (!enabled) {
    releaseSleepDelayInhibit();
    m_sessionProxy.reset();
    m_pendingLockedHint.reset();
    kLog.info("logind session lock monitor disabled");
    return;
  }
  ensureSessionLockMonitor();
}

void LogindService::setLockBeforeSuspendEnabled(bool enabled) {
  if (!m_sessionLockIntegrationEnabled || !enabled) {
    releaseSleepDelayInhibit();
    return;
  }
  (void)acquireSleepDelayInhibit();
}

void LogindService::setPrepareForSleepCallback(PrepareForSleepCallback callback) {
  m_prepareForSleepCallback = std::move(callback);
}

void LogindService::setLockCallback(SessionLockCallback callback) { m_lockCallback = std::move(callback); }

void LogindService::setUnlockCallback(SessionLockCallback callback) { m_unlockCallback = std::move(callback); }

void LogindService::setSessionLockedHint(bool locked) {
  if (!m_sessionLockIntegrationEnabled) {
    return;
  }
  if (m_sessionProxy == nullptr) {
    m_pendingLockedHint = locked;
    return;
  }
  try {
    m_sessionProxy->callMethod("SetLockedHint").onInterface(kLogindSessionInterface).withArguments(locked);
    kLog.debug("logind session locked hint set to {}", locked);
  } catch (const sdbus::Error& e) {
    kLog.warn("failed to set logind session locked hint to {}: {}", locked, e.what());
  }
}

bool LogindService::supportsIdleInhibit() const noexcept { return m_managerProxy != nullptr; }

bool LogindService::hasIdleInhibit() const noexcept { return m_idleInhibitFd >= 0; }

bool LogindService::acquireIdleInhibit() {
  if (m_idleInhibitFd >= 0) {
    return true;
  }
  if (m_managerProxy == nullptr) {
    return false;
  }

  try {
    sdbus::UnixFd fd;
    m_managerProxy->callMethod("Inhibit")
        .onInterface(kLogindManagerInterface)
        .withArguments(std::string("idle"), std::string("noctalia"), std::string("Caffeine"), std::string("block"))
        .storeResultsTo(fd);
    m_idleInhibitFd = fd.release();
    if (m_idleInhibitFd < 0) {
      kLog.warn("logind idle inhibit returned invalid fd");
      return false;
    }
    if (!setCloexec(m_idleInhibitFd)) {
      kLog.warn("failed to set CLOEXEC on logind idle inhibit fd");
      ::close(m_idleInhibitFd);
      m_idleInhibitFd = -1;
      return false;
    }
    kLog.info("logind idle inhibit acquired");
    return true;
  } catch (const sdbus::Error& e) {
    kLog.warn("failed to acquire logind idle inhibit: {}", e.what());
    return false;
  }
}

void LogindService::releaseIdleInhibit() {
  if (m_idleInhibitFd < 0) {
    return;
  }
  ::close(m_idleInhibitFd);
  m_idleInhibitFd = -1;
  kLog.debug("logind idle inhibit released");
}

bool LogindService::hasSleepDelayInhibit() const noexcept { return m_sleepDelayInhibitFd >= 0; }

// The lock is async: a PrepareForSleep before the reply proceeds without the delay lock.
bool LogindService::acquireSleepDelayInhibit() {
  if (m_sleepDelayInhibitFd >= 0) {
    return true;
  }
  if (m_managerProxy == nullptr) {
    return false;
  }
  m_sleepDelayInhibitCancelled = false;
  if (m_sleepDelayInhibitPending) {
    return true;
  }
  m_sleepDelayInhibitPending = true;

  const std::weak_ptr<int> aliveToken = m_aliveToken;
  m_managerProxy->callMethodAsync("Inhibit")
      .onInterface(kLogindManagerInterface)
      .withArguments(
          std::string("sleep"), std::string("noctalia"), std::string("Lock before sleep"), std::string("delay")
      )
      .uponReplyInvoke([this, aliveToken](std::optional<sdbus::Error> e, sdbus::UnixFd fd) {
        if (aliveToken.expired()) {
          return;
        }
        m_sleepDelayInhibitPending = false;
        if (e.has_value()) {
          kLog.warn("failed to acquire logind sleep delay inhibit: {}", e->what());
          return;
        }
        const int rawFd = fd.release();
        if (m_sleepDelayInhibitCancelled) {
          if (rawFd >= 0) {
            ::close(rawFd);
          }
          return;
        }
        if (rawFd < 0) {
          kLog.warn("logind sleep delay inhibit returned invalid fd");
          return;
        }
        if (!setCloexec(rawFd)) {
          kLog.warn("failed to set CLOEXEC on logind sleep delay inhibit fd");
          ::close(rawFd);
          return;
        }
        m_sleepDelayInhibitFd = rawFd;
        kLog.info("logind sleep delay inhibit acquired");
      });
  return true;
}

void LogindService::releaseSleepDelayInhibit() {
  if (m_sleepDelayInhibitPending) {
    m_sleepDelayInhibitCancelled = true;
  }
  if (m_sleepDelayInhibitFd < 0) {
    return;
  }
  ::close(m_sleepDelayInhibitFd);
  m_sleepDelayInhibitFd = -1;
  kLog.info("logind sleep delay inhibit released");
}
