#include "dbus/network/network_lock_client.h"

#include "core/log.h"
#include "dbus/system_bus.h"

#include <optional>
#include <sdbus-c++/IConnection.h>
#include <sdbus-c++/IProxy.h>
#include <sdbus-c++/Message.h>
#include <string_view>
#include <vector>

namespace {

  constexpr Logger kLog("lock");

  const sdbus::ServiceName kNmBusName{"org.freedesktop.NetworkManager"};
  const sdbus::ObjectPath kNmPath{"/org/freedesktop/NetworkManager"};
  constexpr auto kNmInterface = "org.freedesktop.NetworkManager";
  constexpr auto kActiveInterface = "org.freedesktop.NetworkManager.Connection.Active";
  constexpr auto kAccessPointInterface = "org.freedesktop.NetworkManager.AccessPoint";
  constexpr auto kOwnerRule = "type='signal',sender='org.freedesktop.DBus',path='/org/freedesktop/DBus',"
                              "interface='org.freedesktop.DBus',member='NameOwnerChanged',"
                              "arg0='org.freedesktop.NetworkManager'";
  constexpr auto kPropertiesInterface = "org.freedesktop.DBus.Properties";
  // NM_STATE_CONNECTED_LOCAL: the primary connection is activated.
  constexpr std::uint32_t kNmStateConnectedLocal = 50;

  // nullopt for overlay types (vpn, wireguard, tun, ...): they ride on a base link whose kind the glyph shows.
  std::optional<NetworkConnectivity> kindForType(std::string_view type) {
    if (type.empty()) {
      return NetworkConnectivity::Unknown;
    }
    if (type == "802-11-wireless") {
      return NetworkConnectivity::Wireless;
    }
    if (type == "gsm" || type == "cdma") {
      return NetworkConnectivity::Cellular;
    }
    if (type == "802-3-ethernet" || type == "bridge" || type == "bond" || type == "team" || type == "vlan") {
      return NetworkConnectivity::Wired;
    }
    return std::nullopt;
  }

  template <typename T> bool read(const std::map<std::string, sdbus::Variant>& properties, const char* name, T& out) {
    const auto it = properties.find(name);
    if (it == properties.end()) {
      return false;
    }
    try {
      out = it->second.get<T>();
      return true;
    } catch (const sdbus::Error&) {
      return false;
    }
  }

} // namespace

NetworkLockClient::NetworkLockClient(SystemBus& bus) : m_bus(bus) {
  m_ownerMatch = bus.connection().addMatch(
      kOwnerRule,
      [this](sdbus::Message message) {
        std::string name;
        std::string oldOwner;
        std::string newOwner;
        message >> name >> oldOwner >> newOwner;
        m_state = {};
        m_hasState = false;
        notify();
        if (!newOwner.empty()) {
          refresh();
        }
      },
      sdbus::return_slot
  );

  m_nm = sdbus::createProxy(bus.connection(), kNmBusName, kNmPath);
  m_nm->uponSignal("PropertiesChanged")
      .onInterface(kPropertiesInterface)
      .call([this](
                const std::string& interfaceName, const std::map<std::string, sdbus::Variant>& changed,
                const std::vector<std::string>& /*invalidated*/
            ) {
        if (interfaceName != kNmInterface) {
          return;
        }
        // A GetAll sent before a restarted NetworkManager exported its object fails; its signals still carry state.
        const bool first = !m_hasState;
        m_hasState = true;
        if (apply(changed) || first) {
          notify();
        }
        if (m_signalGate && m_signalGate() && m_state.signalStrength == 0) {
          refreshSignal();
        }
      });

  refresh();
}

NetworkLockClient::~NetworkLockClient() = default;

void NetworkLockClient::setChangeCallback(std::function<void()> callback) { m_changeCallback = std::move(callback); }

void NetworkLockClient::setSignalGate(std::function<bool()> gate) { m_signalGate = std::move(gate); }

void NetworkLockClient::refresh() {
  m_nm->callMethodAsync("GetAll")
      .onInterface(kPropertiesInterface)
      .withArguments(std::string{kNmInterface})
      .uponReplyInvoke([this](std::optional<sdbus::Error> error, std::map<std::string, sdbus::Variant> properties) {
        if (error.has_value()) {
          kLog.debug("NetworkManager unavailable: {}", error->what());
          return;
        }
        m_hasState = true;
        (void)apply(properties);
        notify();
        refreshSignal();
      });
}

bool NetworkLockClient::apply(const std::map<std::string, sdbus::Variant>& properties) {
  NetworkState next = m_state;
  if (std::uint32_t state = 0; read(properties, "State", state)) {
    next.connected = state >= kNmStateConnectedLocal;
  }
  bool overlay = false;
  if (std::string type; read(properties, "PrimaryConnectionType", type)) {
    if (const auto kind = kindForType(type); kind.has_value()) {
      next.kind = *kind;
    } else {
      overlay = true;
    }
  }
  (void)read(properties, "WirelessEnabled", next.wirelessEnabled);
  if (next.kind != m_state.kind || next.connected != m_state.connected) {
    next.signalStrength = 0;
  }
  if (overlay && next.kind == NetworkConnectivity::Unknown) {
    resolveKind();
  }
  if (next == m_state) {
    return false;
  }
  m_state = next;
  return true;
}

void NetworkLockClient::resolveKind() {
  m_nm->callMethodAsync("Get")
      .onInterface(kPropertiesInterface)
      .withArguments(std::string{kNmInterface}, std::string{"ActiveConnections"})
      .uponReplyInvoke([this](std::optional<sdbus::Error> error, sdbus::Variant value) {
        if (error.has_value()) {
          return;
        }
        m_activeConnections.clear();
        for (const sdbus::ObjectPath& path : value.get<std::vector<sdbus::ObjectPath>>()) {
          auto& proxy = m_activeConnections.emplace_back(sdbus::createProxy(m_bus.connection(), kNmBusName, path));
          proxy->callMethodAsync("Get")
              .onInterface(kPropertiesInterface)
              .withArguments(std::string{kActiveInterface}, std::string{"Type"})
              .uponReplyInvoke([this](std::optional<sdbus::Error> typeError, sdbus::Variant type) {
                if (typeError.has_value() || m_state.kind != NetworkConnectivity::Unknown) {
                  return;
                }
                const auto kind = kindForType(type.get<std::string>());
                if (!kind.has_value() || *kind == NetworkConnectivity::Unknown) {
                  return;
                }
                m_state.kind = *kind;
                m_state.signalStrength = 0;
                notify();
                refreshSignal();
              });
        }
      });
}

void NetworkLockClient::refreshSignal() {
  if (!m_hasState || m_state.kind != NetworkConnectivity::Wireless || !m_state.connected) {
    return;
  }
  m_nm->callMethodAsync("Get")
      .onInterface(kPropertiesInterface)
      .withArguments(std::string{kNmInterface}, std::string{"PrimaryConnection"})
      .uponReplyInvoke([this](std::optional<sdbus::Error> error, sdbus::Variant value) {
        if (error.has_value()) {
          return;
        }
        m_active = sdbus::createProxy(m_bus.connection(), kNmBusName, value.get<sdbus::ObjectPath>());
        m_active->callMethodAsync("Get")
            .onInterface(kPropertiesInterface)
            .withArguments(std::string{kActiveInterface}, std::string{"SpecificObject"})
            .uponReplyInvoke([this](std::optional<sdbus::Error> activeError, sdbus::Variant accessPoint) {
              if (activeError.has_value()) {
                return;
              }
              m_accessPoint = sdbus::createProxy(m_bus.connection(), kNmBusName, accessPoint.get<sdbus::ObjectPath>());
              m_accessPoint->callMethodAsync("Get")
                  .onInterface(kPropertiesInterface)
                  .withArguments(std::string{kAccessPointInterface}, std::string{"Strength"})
                  .uponReplyInvoke([this](std::optional<sdbus::Error> apError, sdbus::Variant strength) {
                    if (apError.has_value() || m_state.signalStrength == strength.get<std::uint8_t>()) {
                      return;
                    }
                    m_state.signalStrength = strength.get<std::uint8_t>();
                    notify();
                  });
            });
      });
}

void NetworkLockClient::notify() {
  if (m_changeCallback) {
    m_changeCallback();
  }
}
