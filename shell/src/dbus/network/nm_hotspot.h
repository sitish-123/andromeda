#pragma once

#include <memory>
#include <string>

class SystemBus;

namespace sdbus {
  class IProxy;
}

// A Wi-Fi hotspot through NetworkManager: activates the first access-point-mode profile (GNOME's "Hotspot" counts),
// creating one with a shared IPv4 network and a random WPA2 key when none exists.
class NmHotspot {
public:
  explicit NmHotspot(SystemBus& bus);
  ~NmHotspot();

  NmHotspot(const NmHotspot&) = delete;
  NmHotspot& operator=(const NmHotspot&) = delete;

  // Cached; NetworkManager's ActiveConnections change signal invalidates it. The control center asks on every redraw.
  [[nodiscard]] bool active() const;
  // Returns an error message, empty once the request is sent; activation completes asynchronously.
  [[nodiscard]] std::string setEnabled(bool enabled);

private:
  [[nodiscard]] std::string findProfile() const;
  [[nodiscard]] std::string findApDevice() const;
  [[nodiscard]] std::string activeFor(const std::string& profile) const;

  [[nodiscard]] bool queryActive() const;

  SystemBus& m_bus;
  std::unique_ptr<sdbus::IProxy> m_nm;
  mutable bool m_activeKnown = false;
  mutable bool m_active = false;
};
