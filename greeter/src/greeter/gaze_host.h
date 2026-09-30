#pragma once

#include <functional>
#include <gio/gio.h>
#include <string>

// Registers the greeter with gazed as the marker host for the login PAM service, so pam_gaze races the password
// prompt against the face check and gazed tells this process when the face matched. Runs on glib's default
// context, which the greeter loop dispatches under greetd.
class GazeHost {
public:
  using StatusCallback = std::function<void(const std::string& status)>;
  using MatchCallback = std::function<void()>;

  GazeHost() = default;
  ~GazeHost();

  GazeHost(const GazeHost&) = delete;
  GazeHost& operator=(const GazeHost&) = delete;

  void start(StatusCallback onStatus, MatchCallback onMatch);

private:
  static void onSignal(
      GDBusConnection* connection, const char* sender, const char* objectPath, const char* interfaceName,
      const char* signalName, GVariant* parameters, void* userData
  );
  void registerService();

  StatusCallback m_onStatus;
  MatchCallback m_onMatch;
  GDBusConnection* m_connection = nullptr;
  unsigned int m_gazeSubscription = 0;
  unsigned int m_ownerSubscription = 0;
};
