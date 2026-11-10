#pragma once

#include "core/timer_manager.h"
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
// When the card can run an access point beside the connected Wi-Fi client, System1 adds an interface for it and
// a volatile copy of the profile runs there on the client's channel.
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
  // Also true until the first plan reply, so no request takes the wrong path.
  [[nodiscard]] bool pending() const noexcept {
    return state() == HotspotState::Activating || state() == HotspotState::Deactivating || !m_machine.planKnown();
  }
  // The last failed request's message; cleared by the next request or an observed change.
  [[nodiscard]] const std::string& lastError() const noexcept { return m_machine.error(); }
  // The target of the last request; Try Again passes it to setEnabled.
  [[nodiscard]] bool requestedOn() const noexcept { return m_machine.requestedOn(); }
  // Turning the hotspot on now drops the connected Wi-Fi client.
  [[nodiscard]] bool turnsOffWifi() const noexcept { return m_plan.kind == ClientPlan::Kind::TurnsOffClient; }
  // Returns false while a request is in flight or the hotspot is already in that state.
  bool setEnabled(bool enabled);
  // Runs after every state or error change.
  void setChangeCallback(std::function<void()> callback) { m_onChange = std::move(callback); }

private:
  // How a hotspot starts now. TurnsOffClient: the connected Wi-Fi client drops (no second interface on its card,
  // or a 6 GHz client). BesideClient: on a new access-point interface on the client's channel.
  struct ClientPlan {
    enum class Kind : std::uint8_t { NoClient, TurnsOffClient, BesideClient };
    Kind kind = Kind::NoClient;
    std::string client;
    HotspotChannel channel;
  };

  // The access-point interface of the current beside-the-client request, or of an adopted one, and its
  // NetworkManager device. Channel 0: not known (adopted).
  struct ApRun {
    std::string client;
    HotspotChannel channel;
    std::string interface;
    std::string device;
  };

  void notify(bool changed);
  // Notifies, then starts the work the machine's last transition asks for.
  void advance(bool changed);
  void run(HotspotWork work);
  void startQuery();
  void startPlan();
  void setPlan(ClientPlan plan);
  // Watches the client device's ActiveAccessPoint and that access point's Frequency; a change queries the plan.
  void watchClient(const std::string& device, const std::string& ap);
  // Restarts a hotspot beside the client on the plan's channel when the client roamed to another one.
  void followClientChannel();
  void startActivate();
  void finishActivate(std::optional<std::string> error, const std::string& active);
  void startDeactivate();
  void startCreateInterface();
  void startAwaitDevice();
  void checkApDevice();
  void startActivateOnInterface();
  void startRemoveInterface();
  void watchActive(const std::string& path);

  SystemBus& m_bus;
  std::unique_ptr<sdbus::IProxy> m_nm;
  ClientPlan m_plan;
  // Drops plan replies that a newer plan query replaced.
  std::uint64_t m_planToken = 0;
  ApRun m_ap;
  std::unique_ptr<sdbus::IProxy> m_clientDeviceWatch;
  std::unique_ptr<sdbus::IProxy> m_clientApWatch;
  // The hotspot's active connection from the last query; empty when none.
  std::string m_activePath;
  std::unique_ptr<sdbus::IProxy> m_apDeviceWatch;
  Timer m_deviceTimeout;
  // The activated connection; its StateChanged signal ends the Activating state.
  std::unique_ptr<sdbus::IProxy> m_activeWatch;
  // NM's reason from the last StateChanged to Deactivated of the watched connection.
  std::string m_failReason;
  HotspotMachine m_machine;
  std::function<void()> m_onChange;
  std::shared_ptr<int> m_lifetime = std::make_shared<int>(0);
};
