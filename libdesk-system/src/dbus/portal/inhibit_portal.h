#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <sdbus-c++/sdbus-c++.h>
#include <string>
#include <vector>

class SessionBus;
class SystemBus;

// Serves org.freedesktop.impl.portal.Inhibit under org.freedesktop.impl.portal.desktop.noctalia. Each request holds
// logind block inhibitors (idle, sleep) until xdg-desktop-portal closes it; the shell's idle chain already honours
// logind's idle BlockInhibited. Logout and user-switch flags are accepted and not enforced.
class InhibitPortal {
public:
  InhibitPortal(SessionBus& bus, SystemBus* systemBus);
  ~InhibitPortal();

  InhibitPortal(const InhibitPortal&) = delete;
  InhibitPortal& operator=(const InhibitPortal&) = delete;

private:
  struct Handle {
    std::unique_ptr<sdbus::IObject> object;
    std::vector<sdbus::UnixFd> inhibitors;
  };

  void inhibit(const std::string& handle, const std::string& appId, std::uint32_t flags);
  void createMonitor(const std::string& sessionHandle);
  void close(const std::string& handle);

  SessionBus& m_bus;
  std::unique_ptr<sdbus::IObject> m_object;
  std::unique_ptr<sdbus::IProxy> m_logind;
  std::map<std::string, Handle> m_handles;
};
