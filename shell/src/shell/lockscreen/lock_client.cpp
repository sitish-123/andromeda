#include "shell/lockscreen/lock_client.h"

#include "core/log.h"
#include "dbus/session_bus.h"

#include <chrono>
#include <map>
#include <optional>
#include <sdbus-c++/sdbus-c++.h>

namespace {

  constexpr Logger kLog("lock-client");

  const sdbus::ServiceName kDaemonBusName{"org.freedesktop.DBus"};
  const sdbus::ObjectPath kDaemonPath{"/org/freedesktop/DBus"};
  constexpr auto kDaemonInterface = "org.freedesktop.DBus";
  constexpr auto kLockBusName = "org.andromeda.Lock1";
  const sdbus::ObjectPath kLockPath{"/org/andromeda/Lock1"};
  constexpr auto kLockInterface = "org.andromeda.Lock1";
  constexpr auto kPropertiesInterface = "org.freedesktop.DBus.Properties";

} // namespace

LockClient::LockClient(SessionBus& bus) {
  m_daemon = sdbus::createProxy(bus.connection(), kDaemonBusName, kDaemonPath);
  m_daemon->uponSignal("NameOwnerChanged")
      .onInterface(kDaemonInterface)
      .call([this](const std::string& name, const std::string& /*oldOwner*/, const std::string& newOwner) {
        if (name == kLockBusName) {
          onOwner(!newOwner.empty());
        }
      });

  m_lock = sdbus::createProxy(bus.connection(), sdbus::ServiceName{kLockBusName}, kLockPath);
  m_lock->uponSignal("StateChanged").onInterface(kLockInterface).call([this](const std::string& state) {
    if (state == "locked") {
      applyState(true, true);
    } else if (state == "unlocked" || !m_locked) {
      applyState(false, false);
    } else {
      // A failed relock leaves the compositor's blank in place: the session is still locked.
      m_requested = false;
    }
  });
  m_lock->uponSignal("FailedAttempts")
      .onInterface(kLockInterface)
      .call([this](const std::vector<std::int64_t>& rejectedAtMs) {
        if (m_failedAttemptsCallback) {
          m_failedAttemptsCallback(rejectedAtMs);
        }
      });

  std::weak_ptr<int> alive = m_alive;
  m_daemon->callMethodAsync("NameHasOwner")
      .onInterface(kDaemonInterface)
      .withArguments(std::string{kLockBusName})
      .uponReplyInvoke([this, alive](std::optional<sdbus::Error> error, bool owned) {
        if (alive.expired() || m_resolved) {
          return;
        }
        onOwner(!error.has_value() && owned);
      });
}

LockClient::~LockClient() = default;

void LockClient::whenResolved(std::function<void()> fn) {
  if (fn == nullptr) {
    return;
  }
  if (m_resolved) {
    fn();
    return;
  }
  m_whenResolved.push_back(std::move(fn));
}

void LockClient::onOwner(bool owned) {
  m_owned = owned;
  if (owned) {
    if (!m_seen) {
      kLog.info("the lock process serves org.andromeda.Lock1; the shell delegates every lock to it");
    }
    m_seen = true;
    readState();
  } else if (m_seen) {
    kLog.warn("the lock process left the bus; keeping the last lock state until it returns");
  }
  // With an owner, resolution waits for its state (readState), so the first relock sees it.
  if (!m_resolved && !owned) {
    m_resolved = true;
    for (auto& fn : std::exchange(m_whenResolved, {})) {
      fn();
    }
  }
}

void LockClient::readState() {
  std::weak_ptr<int> alive = m_alive;
  m_lock->callMethodAsync("GetAll")
      .onInterface(kPropertiesInterface)
      .withArguments(std::string{kLockInterface})
      .uponReplyInvoke([this, alive](std::optional<sdbus::Error> error, std::map<std::string, sdbus::Variant> props) {
        if (alive.expired()) {
          return;
        }
        if (!error.has_value()) {
          const auto get = [&props](const char* key) {
            const auto it = props.find(key);
            return it != props.end() && it->second.get<bool>();
          };
          // Only StateChanged reports an unlock: a restarted process reads unlocked until its relock lands.
          const bool active = get("Active");
          if (get("Locked")) {
            applyState(true, true);
          } else {
            m_active = active;
          }
          // A lock asked for while the process was away is sent now.
          if (m_requested && !active) {
            lock(m_requestReason);
          }
        } else {
          kLog.warn("cannot read the lock state: {}", error->what());
        }
        if (!m_resolved) {
          m_resolved = true;
          for (auto& fn : std::exchange(m_whenResolved, {})) {
            fn();
          }
        }
      });
}

void LockClient::applyState(bool active, bool locked) {
  const bool wasLocked = m_locked;
  const bool wasActive = m_requested || m_active;
  m_active = active;
  m_locked = locked;
  if (locked || !active) {
    m_requested = false;
  }
  if (!m_stateCallback) {
    return;
  }
  if (locked && !wasLocked) {
    m_stateCallback("locked");
  } else if (!active && wasLocked) {
    m_stateCallback("unlocked");
  } else if (!active && wasActive && !wasLocked) {
    m_stateCallback("aborted");
  }
}

void LockClient::lock(const std::string& reason) {
  // A queued phone lock keeps its reason: it offers no passwordless grace.
  if (!m_requested || reason == "phone") {
    m_requestReason = reason;
  }
  m_requested = true;
  if (!m_owned) {
    return;
  }
  std::weak_ptr<int> alive = m_alive;
  m_lock->callMethodAsync("Lock")
      .onInterface(kLockInterface)
      .withArguments(reason)
      .uponReplyInvoke([this, alive, reason](std::optional<sdbus::Error> error, bool locking) {
        if (alive.expired()) {
          return;
        }
        // An error (the process left mid-call) keeps the request; it is sent again when the process returns.
        if (error.has_value()) {
          kLog.warn("lock ({}) not delivered: {}", reason, error->what());
        } else if (!locking && !m_locked) {
          kLog.warn("lock ({}) refused: the lock screen is off", reason);
          applyState(false, false);
        }
      });
}

void LockClient::call(const char* method) {
  if (m_owned) {
    m_lock->callMethodAsync(method).onInterface(kLockInterface).uponReplyInvoke([](std::optional<sdbus::Error>) {});
  }
}

void LockClient::primeCapture() { call("PrimeCapture"); }

void LockClient::clearCapture() { call("ClearCapture"); }

void LockClient::cancelOutputPowerOff() { call("CancelOutputPowerOff"); }

void LockClient::outputsWoke() { call("OutputsWoke"); }

void LockClient::prepareOutputPowerOff(std::function<void(bool)> done) {
  if (!m_owned) {
    if (done) {
      done(true);
    }
    return;
  }
  m_lock->callMethodAsync("PrepareOutputPowerOff")
      .onInterface(kLockInterface)
      .uponReplyInvoke([done = std::move(done)](std::optional<sdbus::Error> error, bool ready) {
        if (done) {
          done(error.has_value() || ready);
        }
      });
}

// Blocks for the reply: the process must hold the skip before logind's PrepareForSleep, which comes on the system bus.
void LockClient::skipLockOnNextSleep(bool skip) {
  if (!m_owned) {
    return;
  }
  try {
    m_lock->callMethod("SkipLockOnNextSleep")
        .onInterface(kLockInterface)
        .withTimeout(std::chrono::milliseconds(500))
        .withArguments(skip);
  } catch (const sdbus::Error& e) {
    kLog.warn("cannot skip the lock before sleep: {}", e.what());
  }
}
