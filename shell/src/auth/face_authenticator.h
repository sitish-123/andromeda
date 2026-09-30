#pragma once

#include "core/timer_manager.h"

#include <functional>
#include <memory>
#include <string>

class SystemBus;

namespace sdbus {
  class IProxy;
}

// Hands-free face unlock through gazed (com.gundulabs.Gaze). gazed owns the camera and decides the match; this class
// claims it for the current user, runs one verify at a time, and reports progress. See docs/face.md.
class FaceAuthenticator {
public:
  using AuthenticatedCallback = std::function<void()>;
  using StatusCallback = std::function<void(const std::string& message, bool isError)>;

  FaceAuthenticator(SystemBus& bus, std::string user);
  ~FaceAuthenticator();

  FaceAuthenticator(const FaceAuthenticator&) = delete;
  FaceAuthenticator& operator=(const FaceAuthenticator&) = delete;

  void setAuthenticatedCallback(AuthenticatedCallback callback);
  void setStatusCallback(StatusCallback callback);

  // start() begins a new lock and resets the miss count; resume() re-arms after a wrong password without it.
  void start();
  void resume();
  void stop();
  // A key press or pointer button: retries a verify that ended without judging a face.
  void onUserActivity();

private:
  bool ensureProxy();
  void arm();
  void claim();
  void startVerify();
  void stopVerify();
  void release();
  void handleFaceStatus(const std::string& status);
  void handleVerifyStatus(const std::string& result, bool judged, const std::string& rgb, const std::string& ir);
  void handleDaemonGone();
  void emitStatus(const std::string& message, bool isError);

  SystemBus& m_bus;
  std::string m_user;
  std::unique_ptr<sdbus::IProxy> m_gaze;
  std::unique_ptr<sdbus::IProxy> m_dbus;
  std::unique_ptr<sdbus::IProxy> m_loginManager;

  AuthenticatedCallback m_onAuthenticated;
  StatusCallback m_onStatus;

  Timer m_retryTimer;

  bool m_active = false;
  bool m_pending = false;
  bool m_claimed = false;
  bool m_verifying = false;
  bool m_sleeping = false;
  int m_misses = 0;
};
