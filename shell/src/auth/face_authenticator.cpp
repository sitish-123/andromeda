#include "auth/face_authenticator.h"

#include "core/log.h"
#include "dbus/system_bus.h"
#include "i18n/i18n.h"

#include <chrono>
#include <optional>
#include <sdbus-c++/Error.h>
#include <sdbus-c++/IConnection.h>
#include <sdbus-c++/IProxy.h>
#include <sdbus-c++/Types.h>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
  constexpr Logger kLog("face");

  const sdbus::ServiceName kGazeBusName{"com.gundulabs.Gaze"};
  const sdbus::ObjectPath kGazePath{"/com/gundulabs/Gaze"};
  constexpr auto kGazeInterface = "com.gundulabs.Gaze";
  // gazed classifies any service it does not know as a screen lock: no confirmation, lock-screen start delay.
  constexpr auto kPamService = "andromeda-lock";

  const sdbus::ServiceName kDbusBusName{"org.freedesktop.DBus"};
  const sdbus::ObjectPath kDbusPath{"/org/freedesktop/DBus"};
  constexpr auto kDbusInterface = "org.freedesktop.DBus";

  const sdbus::ServiceName kLoginBusName{"org.freedesktop.login1"};
  const sdbus::ObjectPath kLoginPath{"/org/freedesktop/login1"};
  constexpr auto kLoginManagerInterface = "org.freedesktop.login1.Manager";

  constexpr auto kRetryDelay = std::chrono::milliseconds(500);

  using ScoredFace = sdbus::Struct<std::string, double, double, bool, double, double, bool>;

  // callMethodAsync throws, not replies, when the connection itself is gone.
  template <typename Call> bool tryCall(const char* what, Call&& call) {
    try {
      call();
      return true;
    } catch (const sdbus::Error& e) {
      kLog.info("could not call gazed {}: {}", what, e.what());
      return false;
    }
  }

  std::string captureStatusText(const std::string& status) {
    if (status == "no-face") {
      return i18n::tr("auth.face.look");
    }
    if (status == "too-dark") {
      return i18n::tr("auth.face.too-dark");
    }
    if (status == "clipped" || status == "too-close") {
      return i18n::tr("auth.face.move-back");
    }
    if (status == "ready" || status == "usable" || status == "not-centered" || status == "too-far") {
      return i18n::tr("auth.face.hold-still");
    }
    return {};
  }

  // The reason a verify ended without scoring a face, from the more telling of the two cameras.
  std::string unjudgedEndText(const std::string& rgb, const std::string& ir) {
    if (rgb == "too-dark" && (ir == "too-dark" || ir == "unused")) {
      return i18n::tr("auth.face.too-dark-retry");
    }
    if (rgb == "unused" && ir == "unused") {
      return i18n::tr("auth.face.unavailable");
    }
    return i18n::tr("auth.face.no-face-retry");
  }
} // namespace

FaceAuthenticator::FaceAuthenticator(SystemBus& bus, std::string user) : m_bus(bus), m_user(std::move(user)) {
  try {
    m_dbus = sdbus::createProxy(m_bus.connection(), kDbusBusName, kDbusPath);
    m_dbus->uponSignal("NameOwnerChanged")
        .onInterface(kDbusInterface)
        .call([this](const std::string& name, const std::string& /*oldOwner*/, const std::string& newOwner) {
          if (name == kGazeBusName && newOwner.empty()) {
            handleDaemonGone();
          }
        });
  } catch (const sdbus::Error& e) {
    kLog.debug("could not watch gazed ownership: {}", e.what());
    m_dbus.reset();
  }

  try {
    m_loginManager = sdbus::createProxy(m_bus.connection(), kLoginBusName, kLoginPath);
    m_loginManager->uponSignal("PrepareForSleep").onInterface(kLoginManagerInterface).call([this](bool sleeping) {
      run(sleeping ? m_machine.sleep(m_onRequest) : m_machine.wake());
    });
  } catch (const sdbus::Error& e) {
    kLog.debug("could not monitor PrepareForSleep: {}", e.what());
    m_loginManager.reset();
  }
}

FaceAuthenticator::~FaceAuthenticator() { stop(); }

void FaceAuthenticator::setAuthenticatedCallback(AuthenticatedCallback callback) {
  m_onAuthenticated = std::move(callback);
}

void FaceAuthenticator::setStatusCallback(StatusCallback callback) { m_onStatus = std::move(callback); }

void FaceAuthenticator::start() { run(m_machine.start()); }

void FaceAuthenticator::resume() { run(m_machine.resume()); }

void FaceAuthenticator::stop() { run(m_machine.stop()); }

void FaceAuthenticator::request() {
  if (m_machine.phase() == face_auth::Phase::Exhausted) {
    emitStatus(i18n::tr("auth.face.too-many-attempts"), true);
    return;
  }
  if (m_machine.phase() == face_auth::Phase::Unavailable) {
    emitStatus(i18n::tr("auth.face.unavailable"), true);
    return;
  }
  run(m_machine.request());
}

void FaceAuthenticator::onUserActivity() { run(m_machine.activity()); }

bool FaceAuthenticator::ensureProxy() {
  if (m_gaze != nullptr) {
    return true;
  }
  try {
    m_gaze = sdbus::createProxy(m_bus.connection(), kGazeBusName, kGazePath);
    m_gaze->uponSignal("FaceStatus").onInterface(kGazeInterface).call([this](const std::string& status) {
      handleFaceStatus(status);
    });
    m_gaze->uponSignal("VerifyStatus")
        .onInterface(kGazeInterface)
        .call([this](
                  const std::string& result, const std::vector<ScoredFace>& faces, const std::string& rgb,
                  const std::string& ir
              ) {
          if (!m_bus.isNameOwner(kGazeBusName, m_gaze->getCurrentlyProcessedMessage().getSender())) {
            kLog.warn("ignoring VerifyStatus signal not sent by gazed");
            return;
          }
          handleVerifyStatus(result, !faces.empty(), rgb, ir);
        });
  } catch (const sdbus::Error& e) {
    kLog.debug("could not create gazed proxy: {}", e.what());
    m_gaze.reset();
    return false;
  }
  return true;
}

void FaceAuthenticator::run(face_auth::Commands commands) {
  if (m_machine.phase() != face_auth::Phase::RetryWait) {
    m_retryTimer.stop();
  }
  if (commands == face_auth::None) {
    return;
  }
  const std::uint64_t attempt = m_machine.attempt();
  if (m_gaze == nullptr && !ensureProxy()) {
    m_machine.callFailed(attempt);
    return;
  }
  for (const auto [command, method] : {
           std::pair{face_auth::VerifyStop, "VerifyStop"},
           std::pair{face_auth::Release, "Release"},
           std::pair{face_auth::QueryEnrolled, "HasEnrolledFaces"},
           std::pair{face_auth::Claim, "Claim"},
           std::pair{face_auth::VerifyStart, "VerifyStartFor"},
       }) {
    if ((commands & command) != 0) {
      callAsync(method, attempt);
    }
  }
  if ((commands & face_auth::Release) != 0) {
    kLog.info("released gazed");
  }
}

void FaceAuthenticator::callAsync(const char* method, std::uint64_t attempt) {
  const std::string_view name = method;
  if (!tryCall(method, [&]() {
        if (name == "HasEnrolledFaces") {
          m_gaze->callMethodAsync(method)
              .onInterface(kGazeInterface)
              .withArguments(m_user)
              .uponReplyInvoke([this, attempt](std::optional<sdbus::Error> e, bool enrolled) {
                setEnrolled(!e.has_value() && enrolled);
                if (e.has_value()) {
                  kLog.info("face unlock unavailable: {}", e->what());
                } else if (!enrolled) {
                  kLog.info("face unlock off: no face enrolled for {}", m_user);
                }
                run(m_machine.enrolledReply(attempt, !e.has_value() && enrolled));
              });
        } else if (name == "Claim") {
          m_gaze->callMethodAsync(method)
              .onInterface(kGazeInterface)
              .withArguments(m_user)
              .uponReplyInvoke([this, attempt](std::optional<sdbus::Error> e) {
                if (e.has_value()) {
                  kLog.info("could not claim gazed: {}", e->what());
                }
                run(m_machine.claimReply(attempt, !e.has_value()));
              });
        } else if (name == "VerifyStartFor") {
          m_gaze->callMethodAsync(method)
              .onInterface(kGazeInterface)
              .withArguments(std::string{}, std::string{kPamService})
              .uponReplyInvoke([this, attempt](std::optional<sdbus::Error> e) {
                if (e.has_value()) {
                  kLog.info("could not start face verification: {}", e->what());
                }
                run(m_machine.verifyStartReply(attempt, !e.has_value()));
                if (!e.has_value() && m_machine.phase() == face_auth::Phase::Verifying) {
                  kLog.info("face verification started");
                  emitStatus(i18n::tr("auth.face.look"), false);
                }
              });
        } else {
          m_gaze->callMethodAsync(method)
              .onInterface(kGazeInterface)
              .uponReplyInvoke([method](std::optional<sdbus::Error> e) {
                if (e.has_value()) {
                  kLog.debug("gazed {} failed: {}", method, e->what());
                }
              });
        }
      })) {
    m_machine.callFailed(attempt);
  }
}

void FaceAuthenticator::probe() {
  if (!ensureProxy()) {
    setEnrolled(false);
    return;
  }
  if (!tryCall("HasEnrolledFaces", [&]() {
        m_gaze->callMethodAsync("HasEnrolledFaces")
            .onInterface(kGazeInterface)
            .withArguments(m_user)
            .uponReplyInvoke([this](std::optional<sdbus::Error> e, bool enrolled) {
              setEnrolled(!e.has_value() && enrolled);
            });
      })) {
    setEnrolled(false);
  }
}

void FaceAuthenticator::setEnrolled(bool enrolled) {
  if (enrolled == m_enrolled) {
    return;
  }
  m_enrolled = enrolled;
  if (m_onAvailability) {
    m_onAvailability();
  }
}

void FaceAuthenticator::handleFaceStatus(const std::string& status) {
  kLog.debug("face status {} (phase={})", status, static_cast<int>(m_machine.phase()));
  if (m_machine.phase() != face_auth::Phase::Verifying) {
    return;
  }
  const std::string text = captureStatusText(status);
  if (!text.empty()) {
    emitStatus(text, false);
  }
}

void FaceAuthenticator::handleVerifyStatus(
    const std::string& result, bool judged, const std::string& rgb, const std::string& ir
) {
  if (m_machine.phase() != face_auth::Phase::Verifying) {
    return;
  }
  kLog.info("face verify ended: {} judged={} rgb={} ir={}", result, judged, rgb, ir);
  const face_auth::Verdict verdict = result == "verify-match" ? face_auth::Verdict::Match
      : judged                                                ? face_auth::Verdict::Miss
                                                              : face_auth::Verdict::Unjudged;
  run(m_machine.verifyEnded(verdict, m_onRequest));

  if (verdict == face_auth::Verdict::Match) {
    if (m_onAuthenticated) {
      m_onAuthenticated();
    }
    return;
  }
  if (m_machine.phase() == face_auth::Phase::Exhausted) {
    emitStatus(i18n::tr("auth.face.too-many-attempts"), true);
    return;
  }
  emitStatus(judged ? i18n::tr("auth.face.no-match") : unjudgedEndText(rgb, ir), true);
  if (m_machine.phase() == face_auth::Phase::RetryWait) {
    m_retryTimer.start(kRetryDelay, [this]() { run(m_machine.retryDue()); });
  }
}

void FaceAuthenticator::handleDaemonGone() {
  if (m_gaze == nullptr) {
    return;
  }
  kLog.info("gazed left the bus");
  const bool wasVerifying = m_machine.phase() == face_auth::Phase::Verifying;
  m_machine.daemonGone(m_onRequest);
  run(face_auth::None);
  // The signal arrives on another proxy, so dropping this one here cannot free a running handler.
  m_gaze.reset();
  if (wasVerifying) {
    emitStatus(i18n::tr("auth.face.unavailable"), true);
  }
}

void FaceAuthenticator::emitStatus(const std::string& message, bool isError) {
  if (m_onStatus) {
    m_onStatus(message, isError);
  }
}
