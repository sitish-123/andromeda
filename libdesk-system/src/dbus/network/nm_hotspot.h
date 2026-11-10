#pragma once

#include "dbus/network/hotspot_state.h"

#include <functional>
#include <memory>
#include <string>

class SystemBus;

namespace sdbus {
  class IProxy;
}

// A Wi-Fi hotspot through NetworkManager: activates the first access-point-mode profile (GNOME's "Hotspot" counts),
// creating one with a shared IPv4 network and a random WPA2 key when none exists. Every D-Bus call is async.
class NmHotspot {
public:
  explicit NmHotspot(SystemBus& bus);
  ~NmHotspot();

  NmHotspot(const NmHotspot&) = delete;
  NmHotspot& operator=(const NmHotspot&) = delete;

  // Cached; never blocks. NetworkManager's ActiveConnections signal refreshes it.
  [[nodiscard]] HotspotState state() const noexcept { return m_machine.state(); }
  // The requested state while a request is in flight, else the real one.
  [[nodiscard]] bool active() const noexcept {
    return state() == HotspotState::On || state() == HotspotState::Activating;
  }
  [[nodiscard]] bool pending() const noexcept {
    return state() == HotspotState::Activating || state() == HotspotState::Deactivating;
  }
  // The last failed request's message; cleared by the next request or an observed change.
  [[nodiscard]] const std::string& lastError() const noexcept { return m_machine.error(); }
  // The target of the last request; Try Again passes it to setEnabled.
  [[nodiscard]] bool requestedOn() const noexcept { return m_machine.requestedOn(); }
  // Returns false while a request is in flight or the hotspot is already in that state.
  bool setEnabled(bool enabled);
  // Runs after every state or error change.
  void setChangeCallback(std::function<void()> callback) { m_onChange = std::move(callback); }

private:
  void notify(bool changed);
  void startQuery();
  void startActivate();
  void startDeactivate();
  void watchActive(const std::string& path);

  SystemBus& m_bus;
  std::unique_ptr<sdbus::IProxy> m_nm;
  // The activated connection; its StateChanged signal ends the Activating state.
  std::unique_ptr<sdbus::IProxy> m_activeWatch;
  // NM's reason from the last StateChanged to Deactivated of the watched connection.
  std::string m_failReason;
  HotspotMachine m_machine;
  std::function<void()> m_onChange;
  std::shared_ptr<int> m_lifetime = std::make_shared<int>(0);
};
