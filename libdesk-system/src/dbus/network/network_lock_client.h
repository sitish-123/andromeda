#pragma once

#include "dbus/network/network_types.h"

#include <functional>
#include <map>
#include <memory>
#include <sdbus-c++/Types.h>
#include <string>
#include <vector>

class SystemBus;

namespace sdbus {
  class IProxy;
} // namespace sdbus

// Read-only NetworkManager view for `noctalia lock`'s status glyph: no scans, no secret agent, no polling. The Wi-Fi
// signal strength is read only on refreshSignal() and, while the signal gate is open, when Wi-Fi connects, so
// access-point updates never wake the process.
class NetworkLockClient {
public:
  explicit NetworkLockClient(SystemBus& bus);
  ~NetworkLockClient();

  void setChangeCallback(std::function<void()> callback);
  void setSignalGate(std::function<bool()> gate);
  void refreshSignal();
  [[nodiscard]] bool hasState() const noexcept { return m_hasState; }
  [[nodiscard]] const NetworkState& state() const noexcept { return m_state; }

private:
  void refresh();
  [[nodiscard]] bool apply(const std::map<std::string, sdbus::Variant>& properties);
  void resolveKind();
  void notify();

  SystemBus& m_bus;
  sdbus::Slot m_ownerMatch;
  std::unique_ptr<sdbus::IProxy> m_nm;
  std::unique_ptr<sdbus::IProxy> m_active;
  std::unique_ptr<sdbus::IProxy> m_accessPoint;
  std::vector<std::unique_ptr<sdbus::IProxy>> m_activeConnections;
  NetworkState m_state;
  bool m_hasState = false;
  std::function<void()> m_changeCallback;
  std::function<bool()> m_signalGate;
};
