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
    apply(LockClientState::StateChanged{.state = state});
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
        if (alive.expired() || m_state.resolved()) {
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
  if (m_state.resolved()) {
    fn();
    return;
  }
  m_whenResolved.push_back(std::move(fn));
}

void LockClient::onOwner(bool owned) {
  if (owned && !m_state.seen()) {
    kLog.info("the lock process serves org.andromeda.Lock1; the shell delegates every lock to it");
  } else if (!owned && m_state.seen()) {
    kLog.warn("the lock process left the bus; keeping the last lock state until it returns");
  }
  // With an owner, resolution waits for its state (readState), so the first relock sees it.
  apply(LockClientState::OwnerChanged{.owned = owned});
}

void LockClient::apply(LockClientState::Event event) {
  const LockClientState::Result result = m_state.apply(std::move(event));
  if (m_stateCallback) {
    switch (result.notice) {
    case LockClientState::Notice::Locked:
      m_stateCallback("locked");
      break;
    case LockClientState::Notice::Unlocked:
      m_stateCallback("unlocked");
      break;
    case LockClientState::Notice::Aborted:
      m_stateCallback("aborted");
      break;
    case LockClientState::Notice::None:
      break;
    }
  }
  if (result.sendLock.has_value()) {
    sendLock(*result.sendLock);
  }
  if (result.readState) {
    readState();
  }
  if (result.resolved) {
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
        if (error.has_value()) {
          kLog.warn("cannot read the lock state: {}", error->what());
          apply(LockClientState::StateRead{});
          return;
        }
        const auto get = [&props](const char* key) {
          const auto it = props.find(key);
          return it != props.end() && it->second.get<bool>();
        };
        apply(LockClientState::StateRead{.ok = true, .active = get("Active"), .locked = get("Locked")});
      });
}

void LockClient::lock(const std::string& reason) { apply(LockClientState::LockAsked{.reason = reason}); }

void LockClient::sendLock(const std::string& reason) {
  if (!m_state.owned()) {
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
        } else if (!locking && !m_state.locked()) {
          kLog.warn("lock ({}) refused: the lock screen is off", reason);
          apply(LockClientState::LockRefused{});
        }
      });
}

void LockClient::call(const char* method) {
  if (m_state.owned()) {
    m_lock->callMethodAsync(method).onInterface(kLockInterface).uponReplyInvoke([](std::optional<sdbus::Error>) {});
  }
}

void LockClient::primeCapture() { call("PrimeCapture"); }

void LockClient::clearCapture() { call("ClearCapture"); }

void LockClient::cancelOutputPowerOff() { call("CancelOutputPowerOff"); }

void LockClient::outputsWoke() { call("OutputsWoke"); }

void LockClient::prepareOutputPowerOff(std::function<void(bool)> done) {
  if (!m_state.owned()) {
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
  if (!m_state.owned()) {
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
