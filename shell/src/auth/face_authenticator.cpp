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
#include <utility>
#include <vector>

namespace {
  constexpr Logger kLog("face");

  const sdbus::ServiceName kGazeBusName{"com.gundulabs.Gaze"};
  const sdbus::ObjectPath kGazePath{"/com/gundulabs/Gaze"};
  constexpr auto kGazeInterface = "com.gundulabs.Gaze";
  // gazed classifies any service it does not know as a screen lock: no confirmation, lock-screen start delay.
  constexpr auto kPamService = "umbriel-lock";

  const sdbus::ServiceName kDbusBusName{"org.freedesktop.DBus"};
  const sdbus::ObjectPath kDbusPath{"/org/freedesktop/DBus"};
  constexpr auto kDbusInterface = "org.freedesktop.DBus";

  const sdbus::ServiceName kLoginBusName{"org.freedesktop.login1"};
  const sdbus::ObjectPath kLoginPath{"/org/freedesktop/login1"};
  constexpr auto kLoginManagerInterface = "org.freedesktop.login1.Manager";

  constexpr int kMaxMisses = 3;
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
      m_sleeping = sleeping;
      if (sleeping) {
        m_retryTimer.stop();
        stopVerify();
      } else if (m_active) {
        arm();
      }
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

void FaceAuthenticator::start() {
  if (m_active) {
    return;
  }
  m_active = true;
  m_misses = 0;
  arm();
}

void FaceAuthenticator::resume() {
  if (m_active || m_misses >= kMaxMisses) {
    return;
  }
  m_active = true;
  arm();
}

void FaceAuthenticator::stop() {
  m_retryTimer.stop();
  m_active = false;
  m_pending = false;
  stopVerify();
  release();
}

void FaceAuthenticator::onUserActivity() {
  if (m_active && !m_verifying && !m_pending && !m_sleeping && m_misses < kMaxMisses) {
    arm();
  }
}

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
              ) { handleVerifyStatus(result, !faces.empty(), rgb, ir); });
  } catch (const sdbus::Error& e) {
    kLog.debug("could not create gazed proxy: {}", e.what());
    m_gaze.reset();
    return false;
  }
  return true;
}

void FaceAuthenticator::arm() {
  if (!m_active || m_sleeping || m_pending || m_verifying || !ensureProxy()) {
    return;
  }
  m_pending = true;
  if (!tryCall("HasEnrolledFaces", [&]() {
        m_gaze->callMethodAsync("HasEnrolledFaces")
            .onInterface(kGazeInterface)
            .withArguments(m_user)
            .uponReplyInvoke([this](std::optional<sdbus::Error> e, bool enrolled) {
              if (e.has_value()) {
                m_pending = false;
                kLog.info("face unlock unavailable: {}", e->what());
                return;
              }
              if (!enrolled) {
                m_pending = false;
                kLog.info("face unlock off: no face enrolled for {}", m_user);
                return;
              }
              if (m_claimed) {
                startVerify();
              } else {
                claim();
              }
            });
      })) {
    m_pending = false;
  }
}

void FaceAuthenticator::claim() {
  if (!tryCall("Claim", [&]() {
        m_gaze->callMethodAsync("Claim")
            .onInterface(kGazeInterface)
            .withArguments(m_user)
            .uponReplyInvoke([this](std::optional<sdbus::Error> e) {
              if (e.has_value()) {
                m_pending = false;
                kLog.info("could not claim gazed: {}", e->what());
                return;
              }
              m_claimed = true;
              if (!m_active) {
                m_pending = false;
                release();
                return;
              }
              startVerify();
            });
      })) {
    m_pending = false;
  }
}

void FaceAuthenticator::startVerify() {
  if (!m_active || m_sleeping) {
    m_pending = false;
    return;
  }
  if (!tryCall("VerifyStartFor", [&]() {
        m_gaze->callMethodAsync("VerifyStartFor")
            .onInterface(kGazeInterface)
            .withArguments(std::string{}, std::string{kPamService})
            .uponReplyInvoke([this](std::optional<sdbus::Error> e) {
              m_pending = false;
              if (e.has_value()) {
                kLog.info("could not start face verification: {}", e->what());
                return;
              }
              m_verifying = true;
              kLog.info("face verification started");
              emitStatus(i18n::tr("auth.face.look"), false);
            });
      })) {
    m_pending = false;
  }
}

void FaceAuthenticator::stopVerify() {
  if (!m_verifying || m_gaze == nullptr) {
    m_verifying = false;
    return;
  }
  m_verifying = false;
  tryCall("VerifyStop", [&]() {
    m_gaze->callMethodAsync("VerifyStop")
        .onInterface(kGazeInterface)
        .uponReplyInvoke([](std::optional<sdbus::Error> e) {
          if (e.has_value()) {
            kLog.debug("could not stop face verification: {}", e->what());
          }
        });
  });
}

void FaceAuthenticator::release() {
  if (!m_claimed || m_gaze == nullptr) {
    m_claimed = false;
    return;
  }
  m_claimed = false;
  tryCall("Release", [&]() {
    m_gaze->callMethodAsync("Release").onInterface(kGazeInterface).uponReplyInvoke([](std::optional<sdbus::Error> e) {
      if (e.has_value()) {
        kLog.debug("could not release gazed: {}", e->what());
      }
    });
  });
  kLog.info("released gazed");
}

void FaceAuthenticator::handleFaceStatus(const std::string& status) {
  kLog.debug("face status {} (active={} verifying={})", status, m_active, m_verifying);
  if (!m_active || !m_verifying) {
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
  if (!m_active || !m_verifying) {
    return;
  }
  m_verifying = false;
  kLog.info("face verify ended: {} judged={} rgb={} ir={}", result, judged, rgb, ir);

  if (result == "verify-match") {
    release();
    if (m_onAuthenticated) {
      m_onAuthenticated();
    }
    return;
  }

  if (!judged) {
    emitStatus(unjudgedEndText(rgb, ir), true);
    return;
  }

  ++m_misses;
  if (m_misses >= kMaxMisses) {
    emitStatus(i18n::tr("auth.face.too-many-attempts"), true);
    release();
    return;
  }
  emitStatus(i18n::tr("auth.face.no-match"), true);
  m_retryTimer.start(kRetryDelay, [this]() { arm(); });
}

void FaceAuthenticator::handleDaemonGone() {
  if (m_gaze == nullptr) {
    return;
  }
  kLog.info("gazed left the bus");
  const bool wasVerifying = m_verifying;
  m_pending = false;
  m_claimed = false;
  m_verifying = false;
  // The signal arrives on another proxy, so dropping this one here cannot free a running handler.
  m_gaze.reset();
  if (wasVerifying && m_active) {
    emitStatus(i18n::tr("auth.face.unavailable"), true);
  }
}

void FaceAuthenticator::emitStatus(const std::string& message, bool isError) {
  if (m_onStatus) {
    m_onStatus(message, isError);
  }
}
