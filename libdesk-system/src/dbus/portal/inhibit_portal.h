#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <sdbus-c++/sdbus-c++.h>
#include <string>
#include <vector>

class SessionBus;
class SystemBus;

// Serves org.freedesktop.impl.portal.Inhibit under org.freedesktop.impl.portal.desktop.noctalia. Each request holds
// logind block inhibitors (idle, sleep) until xdg-desktop-portal closes it; the shell's idle chain already honours
// logind's idle BlockInhibited. Logout requests are kept with their reason for the session end, which lists them and
// tells monitors Query End, then Ending (session-state 2, 3), so apps can save first.
class InhibitPortal {
public:
  InhibitPortal(SessionBus& bus, SystemBus* systemBus);
  ~InhibitPortal();

  InhibitPortal(const InhibitPortal&) = delete;
  InhibitPortal& operator=(const InhibitPortal&) = delete;

  enum class SessionState : std::uint32_t { Running = 1, QueryEnd = 2, Ending = 3 };
  struct LogoutInhibitor {
    std::string appId;
    std::string reason;
  };
  [[nodiscard]] std::vector<LogoutInhibitor> logoutInhibitors() const;
  void setSessionState(SessionState state);
  // Runs when a logout inhibitor comes or goes.
  void setChangeCallback(std::function<void()> callback) { m_changeCallback = std::move(callback); }

private:
  struct Handle {
    std::unique_ptr<sdbus::IObject> object;
    std::vector<sdbus::UnixFd> inhibitors;
    std::string appId;
    std::string reason;
    std::uint32_t flags = 0;
    bool monitor = false;
  };

  void inhibit(const std::string& handle, const std::string& appId, std::uint32_t flags, const std::string& reason);
  void emitState(const std::string& sessionHandle);
  void createMonitor(const std::string& sessionHandle);
  void close(const std::string& handle);

  SessionBus& m_bus;
  std::unique_ptr<sdbus::IObject> m_object;
  std::unique_ptr<sdbus::IProxy> m_logind;
  std::map<std::string, Handle> m_handles;
  SessionState m_state = SessionState::Running;
  std::function<void()> m_changeCallback;
};
