#include "auth/fingerprint_authenticator.h"

#include "core/log.h"
#include "dbus/system_bus.h"
#include "i18n/i18n.h"

#include <chrono>
#include <map>
#include <optional>
#include <sdbus-c++/Error.h>
#include <sdbus-c++/IConnection.h>
#include <sdbus-c++/IProxy.h>
#include <sdbus-c++/Types.h>
#include <string>
#include <utility>

namespace {
  constexpr Logger kLog("fingerprint");

  const sdbus::ServiceName kFprintBusName{"net.reactivated.Fprint"};
  const sdbus::ObjectPath kManagerPath{"/net/reactivated/Fprint/Manager"};
  constexpr auto kManagerInterface = "net.reactivated.Fprint.Manager";
  constexpr auto kDeviceInterface = "net.reactivated.Fprint.Device";
  constexpr auto kPropertiesInterface = "org.freedesktop.DBus.Properties";

  const sdbus::ServiceName kLoginBusName{"org.freedesktop.login1"};
  const sdbus::ObjectPath kLoginPath{"/org/freedesktop/login1"};
  constexpr auto kLoginManagerInterface = "org.freedesktop.login1.Manager";

  constexpr auto kRetryDelay = std::chrono::milliseconds(250);

  enum class MatchResult {
    Invalid,
    NoMatch,
    Matched,
    Retry,
    SwipeTooShort,
    FingerNotCentered,
    RemoveAndRetry,
    Disconnected,
    UnknownError,
  };

  MatchResult parseVerifyResult(const std::string& result) {
    static const std::map<std::string, MatchResult> kResults = {
        {"verify-no-match", MatchResult::NoMatch},
        {"verify-match", MatchResult::Matched},
        {"verify-retry-scan", MatchResult::Retry},
        {"verify-swipe-too-short", MatchResult::SwipeTooShort},
        {"verify-finger-not-centered", MatchResult::FingerNotCentered},
        {"verify-remove-and-retry", MatchResult::RemoveAndRetry},
        {"verify-disconnected", MatchResult::Disconnected},
        {"verify-unknown-error", MatchResult::UnknownError},
    };
    const auto it = kResults.find(result);
    return it == kResults.end() ? MatchResult::Invalid : it->second;
  }

  bool isRecoverableVerifyStartError(const sdbus::Error& error) {
    // Suspend/resume can drop the claim while the proxy survives; only that case is worth
    // recreating+reclaiming. Other VerifyStart failures are permanent for this lock attempt.
    return error.getName() == sdbus::Error::Name{"net.reactivated.Fprint.Error.ClaimDevice"};
  }
} // namespace

using fingerprint_auth::Phase;

FingerprintAuthenticator::FingerprintAuthenticator(SystemBus& bus) : m_bus(bus) {
  try {
    m_loginManager = sdbus::createProxy(m_bus.connection(), kLoginBusName, kLoginPath);
    m_loginManager->uponSignal("PrepareForSleep").onInterface(kLoginManagerInterface).call([this](bool sleeping) {
      run(sleeping ? m_machine.sleep() : m_machine.wake());
    });
  } catch (const sdbus::Error& e) {
    kLog.debug("could not monitor PrepareForSleep: {}", e.what());
    m_loginManager.reset();
  }
}

FingerprintAuthenticator::~FingerprintAuthenticator() { stop(); }

void FingerprintAuthenticator::setAuthenticatedCallback(AuthenticatedCallback callback) {
  m_onAuthenticated = std::move(callback);
}

void FingerprintAuthenticator::setStatusCallback(StatusCallback callback) { m_onStatus = std::move(callback); }

void FingerprintAuthenticator::start() { run(m_machine.start()); }

// Release even after verify-disconnected: fprintd also reports libfprint's recoverable TOO_HOT
// cutoff that way and keeps our claim until Release or until this long-lived connection closes.
void FingerprintAuthenticator::stop() { run(m_machine.stop()); }

void FingerprintAuthenticator::run(fingerprint_auth::Commands commands) {
  const std::uint64_t attempt = m_machine.attempt();
  if ((commands & fingerprint_auth::VerifyStop) != 0) {
    stopVerify();
  }
  if ((commands & fingerprint_auth::Release) != 0) {
    releaseDevice();
  }
  if ((commands & fingerprint_auth::RequestDevice) != 0) {
    requestDevice(attempt);
  }
  if ((commands & fingerprint_auth::Claim) != 0) {
    claimDevice(attempt);
  }
  if ((commands & fingerprint_auth::VerifyStart) != 0) {
    startVerify(attempt);
  }
  const Phase phase = m_machine.phase();
  if (phase != Phase::RetryWait && phase != Phase::ReclaimWait) {
    m_retryTimer.stop();
  } else if (!m_retryTimer.active()) {
    m_retryTimer.start(kRetryDelay, [this] { run(m_machine.retryDue()); });
  }
}

// Async: GetDefaultDevice D-Bus-activates fprintd, which can take seconds; the lock screen must not wait for it.
void FingerprintAuthenticator::requestDevice(std::uint64_t attempt) {
  try {
    m_manager = sdbus::createProxy(m_bus.connection(), kFprintBusName, kManagerPath);
    m_manager->callMethodAsync("GetDefaultDevice")
        .onInterface(kManagerInterface)
        .uponReplyInvoke([this, attempt](std::optional<sdbus::Error> e, sdbus::ObjectPath path) {
          if (!m_machine.current(attempt, Phase::RequestingDevice)) {
            return;
          }
          if (e.has_value()) {
            kLog.info("no fprintd device available: {}", e->what());
            setAvailable(false);
          }
          const bool ok = !e.has_value() && createDeviceProxy(path);
          if (ok) {
            emitStatus(i18n::tr("auth.fingerprint.ready"), false);
          }
          run(m_machine.deviceReply(attempt, ok));
        });
  } catch (const sdbus::Error& e) {
    kLog.info("no fprintd device available: {}", e.what());
    m_machine.callFailed(attempt);
    setAvailable(false);
  }
}

bool FingerprintAuthenticator::createDeviceProxy(const std::string& path) {
  kLog.info("using fingerprint device {}", path.c_str());
  try {
    m_device = sdbus::createProxy(m_bus.connection(), kFprintBusName, sdbus::ObjectPath(path));
  } catch (const sdbus::Error& e) {
    kLog.warn("could not create device proxy: {}", e.what());
    m_device.reset();
    return false;
  }

  m_device->uponSignal("VerifyStatus").onInterface(kDeviceInterface).call([this](const std::string& result, bool done) {
    if (!m_bus.isNameOwner(kFprintBusName, m_device->getCurrentlyProcessedMessage().getSender())) {
      kLog.warn("ignoring VerifyStatus signal not sent by fprintd");
      return;
    }
    handleVerifyStatus(result, done);
  });

  m_device->uponSignal("PropertiesChanged")
      .onInterface(kPropertiesInterface)
      .call([this](
                const std::string& interfaceName, const std::map<std::string, sdbus::Variant>& changed,
                const std::vector<std::string>& /*invalidated*/
            ) {
        if (interfaceName != kDeviceInterface) {
          return;
        }
        const auto it = changed.find("finger-present");
        if (it == changed.end()) {
          return;
        }
        try {
          if (it->second.get<bool>()) {
            emitStatus(i18n::tr("auth.fingerprint.present"), false);
          }
        } catch (const sdbus::Error&) {
        }
      });

  return true;
}

void FingerprintAuthenticator::claimDevice(std::uint64_t attempt) {
  try {
    m_device->callMethodAsync("Claim")
        .onInterface(kDeviceInterface)
        .withArguments(std::string{})
        .uponReplyInvoke([this](std::optional<sdbus::Error> e) {
          const bool waiting = m_machine.phase() == Phase::Claiming;
          if (e.has_value()) {
            kLog.info("could not claim fingerprint device: {}", e->what());
          } else {
            kLog.info("claimed fingerprint device");
          }
          run(m_machine.claimReply(!e.has_value()));
          if (e.has_value() && waiting) {
            setAvailable(false);
          }
        });
  } catch (const sdbus::Error& e) {
    kLog.info("could not claim fingerprint device: {}", e.what());
    m_machine.callFailed(attempt);
    setAvailable(false);
  }
}

void FingerprintAuthenticator::startVerify(std::uint64_t attempt) {
  try {
    m_device->callMethodAsync("VerifyStart")
        .onInterface(kDeviceInterface)
        .withArguments(std::string{"any"})
        .uponReplyInvoke([this, attempt](std::optional<sdbus::Error> e) {
          if (!m_machine.current(attempt, Phase::StartingVerify)) {
            return;
          }
          // A claim dropped across suspend/resume makes VerifyStart fail with ClaimDevice; the machine then
          // requests and claims the device once more.
          const bool claimLost = e.has_value() && isRecoverableVerifyStartError(*e);
          if (e.has_value()) {
            kLog.info("could not start fingerprint verification: {}", e->what());
          }
          run(m_machine.verifyStartReply(attempt, !e.has_value(), claimLost));
          if (m_machine.phase() == Phase::Verifying) {
            kLog.debug("fingerprint verification started");
            setAvailable(true);
            emitStatus(i18n::tr("auth.fingerprint.ready"), false);
          } else if (m_machine.phase() == Phase::Idle) {
            setAvailable(false);
            emitStatus({}, false); // issue loading: drop the status message
          }
        });
  } catch (const sdbus::Error& e) {
    kLog.info("could not start fingerprint verification: {}", e.what());
    m_machine.callFailed(attempt);
    setAvailable(false);
  }
}

void FingerprintAuthenticator::setAvailable(bool available) {
  if (available == m_available) {
    return;
  }
  m_available = available;
  if (m_onAvailability) {
    m_onAvailability();
  }
}

void FingerprintAuthenticator::stopVerify() {
  if (m_device == nullptr) {
    return;
  }
  // Async: fprintd can take seconds while the sensor is busy, and the lock screen must not wait for it.
  try {
    m_device->callMethodAsync("VerifyStop")
        .onInterface(kDeviceInterface)
        .uponReplyInvoke([](std::optional<sdbus::Error> e) {
          if (e) {
            kLog.debug("could not stop fingerprint verification: {}", e->what());
          }
        });
  } catch (const sdbus::Error& e) {
    kLog.debug("could not stop fingerprint verification: {}", e.what());
  }
}

// The proxy stays until the next device reply replaces it: this can run inside one of its own reply handlers.
void FingerprintAuthenticator::releaseDevice() {
  try {
    m_device->callMethodAsync("Release").onInterface(kDeviceInterface).uponReplyInvoke([](std::optional<sdbus::Error>) {
    });
    kLog.info("released fingerprint device");
  } catch (const sdbus::Error& e) {
    kLog.debug("could not release fingerprint device: {}", e.what());
  }
}

void FingerprintAuthenticator::handleVerifyStatus(const std::string& result, bool done) {
  const bool verifying = m_machine.phase() == Phase::Verifying;
  kLog.debug("verify status: {} (done={})", result, done);
  const MatchResult match = parseVerifyResult(result);

  fingerprint_auth::Status status = fingerprint_auth::Status::InPlace;
  switch (match) {
  case MatchResult::Invalid:
    status = fingerprint_auth::Status::Invalid;
    break;
  case MatchResult::Matched:
    status = fingerprint_auth::Status::Match;
    break;
  case MatchResult::NoMatch:
    status = fingerprint_auth::Status::NoMatch;
    break;
  case MatchResult::Disconnected:
    status = fingerprint_auth::Status::Disconnected;
    break;
  case MatchResult::UnknownError:
    status = fingerprint_auth::Status::Error;
    break;
  default:
    break;
  }
  run(m_machine.verifyStatus(status, done));
  if (!verifying) {
    return;
  }

  const bool exhausted = m_machine.phase() == Phase::Idle;
  switch (match) {
  case MatchResult::Invalid:
    kLog.warn("unknown fingerprint verify status: {}", result);
    break;
  case MatchResult::Matched:
    if (m_onAuthenticated) {
      m_onAuthenticated();
    }
    break;
  case MatchResult::NoMatch:
    emitStatus(i18n::tr(exhausted ? "auth.fingerprint.too-many-attempts" : "auth.fingerprint.no-match"), true);
    break;
  case MatchResult::Retry:
    emitStatus(i18n::tr("auth.fingerprint.retry"), false);
    break;
  case MatchResult::SwipeTooShort:
    emitStatus(i18n::tr("auth.fingerprint.swipe-too-short"), false);
    break;
  case MatchResult::FingerNotCentered:
    emitStatus(i18n::tr("auth.fingerprint.not-centered"), false);
    break;
  case MatchResult::RemoveAndRetry:
    emitStatus(i18n::tr("auth.fingerprint.remove-and-retry"), false);
    break;
  case MatchResult::Disconnected:
    emitStatus(i18n::tr("auth.fingerprint.disconnected"), true);
    break;
  case MatchResult::UnknownError:
    emitStatus(i18n::tr(exhausted ? "auth.fingerprint.too-many-attempts" : "auth.fingerprint.error"), true);
    break;
  }
}


void FingerprintAuthenticator::emitStatus(const std::string& message, bool isError) {
  if (m_onStatus) {
    m_onStatus(message, isError);
  }
}
