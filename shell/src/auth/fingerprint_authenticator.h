#pragma once

#include "core/timer_manager.h"

#include <functional>
#include <memory>
#include <string>

class SystemBus;

namespace sdbus {
  class IProxy;
}

class FingerprintAuthenticator {
public:
  using AuthenticatedCallback = std::function<void()>;
  using StatusCallback = std::function<void(const std::string& message, bool isError)>;
  using AvailabilityCallback = std::function<void()>;

  explicit FingerprintAuthenticator(SystemBus& bus);
  ~FingerprintAuthenticator();

  FingerprintAuthenticator(const FingerprintAuthenticator&) = delete;
  FingerprintAuthenticator& operator=(const FingerprintAuthenticator&) = delete;

  void setAuthenticatedCallback(AuthenticatedCallback callback);
  void setStatusCallback(StatusCallback callback);
  void setAvailabilityCallback(AvailabilityCallback callback) { m_onAvailability = std::move(callback); }
  // A sensor is claimed and verifying, so a finger is enrolled (false until fprintd answers).
  [[nodiscard]] bool available() const noexcept { return m_available; }

  void start();
  void stop();

private:
  void setAvailable(bool available);
  // Asks fprintd for its default device without blocking; on the reply it sets up m_device and claims it.
  void requestDevice();
  bool createDeviceProxy(const std::string& path);
  void claimDevice();
  void startVerify(bool isRetry);
  void stopVerify();
  void releaseDevice();
  void handleVerifyStatus(const std::string& result, bool done);
  void emitStatus(const std::string& message, bool isError);

  SystemBus& m_bus;
  std::unique_ptr<sdbus::IProxy> m_loginManager;
  std::unique_ptr<sdbus::IProxy> m_device;
  std::unique_ptr<sdbus::IProxy> m_manager; // alive while GetDefaultDevice is pending

  AuthenticatedCallback m_onAuthenticated;
  StatusCallback m_onStatus;

  Timer m_retryTimer;

  bool m_active = false;
  bool m_available = false;
  AvailabilityCallback m_onAvailability;
  bool m_verifying = false;
  bool m_claiming = false;
  bool m_requestingDevice = false;
  bool m_sleeping = false;
  bool m_abort = false;
  bool m_reclaimAttempted = false;
  int m_retries = 0;
};
