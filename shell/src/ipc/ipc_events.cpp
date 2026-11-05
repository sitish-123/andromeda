#include "ipc/ipc_events.h"

#include "dbus/bluetooth/bluetooth_service.h"
#include "dbus/mpris/mpris_service.h"
#include "dbus/network/network_types.h"
#include "dbus/power/power_profiles_service.h"
#include "ipc/ipc_service.h"

#include <map>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

namespace ipc_events {

  namespace {
    IpcService* g_sink = nullptr;

    void send(std::string_view topic, nlohmann::json data) {
      if (g_sink == nullptr) {
        return;
      }
      static std::map<std::string, nlohmann::json, std::less<>> last;
      auto it = last.find(topic);
      if (it != last.end() && it->second == data) {
        return;
      }
      g_sink->publish(topic, data);
      last.insert_or_assign(std::string(topic), std::move(data));
    }

    const char* connectivityName(NetworkConnectivity kind) {
      switch (kind) {
      case NetworkConnectivity::Wireless:
        return "wireless";
      case NetworkConnectivity::Wired:
        return "wired";
      case NetworkConnectivity::Cellular:
        return "cellular";
      case NetworkConnectivity::None:
        return "none";
      case NetworkConnectivity::Unknown:
        break;
      }
      return "unknown";
    }
  } // namespace

  void setSink(IpcService* ipc) { g_sink = ipc; }

  nlohmann::json toJson(const BluetoothState& state) {
    return {
        {"adapter", state.adapterPresent},
        {"adapter_name", state.adapterName},
        {"powered", state.powered},
        {"blocked", state.rfkillSoftBlocked},
        {"discovering", state.discovering},
    };
  }

  nlohmann::json toJson(const NetworkState& state) {
    return {
        {"kind", connectivityName(state.kind)},
        {"connected", state.connected},
        {"connecting", state.resolving},
        {"wifi_enabled", state.wirelessEnabled},
        {"ssid", state.ssid},
        {"signal", state.signalStrength},
        {"interface", state.interfaceName},
        {"ipv4", state.ipv4},
        {"vpn", state.vpnConnected},
    };
  }

  nlohmann::json toJson(const PowerProfilesState& state) {
    return {
        {"active", state.activeProfile},
        {"profiles", state.profiles},
        {"performance_inhibited", state.performanceInhibited},
    };
  }

  nlohmann::json mediaJson(const MprisService& service) {
    const auto player = service.activePlayer();
    if (!player) {
      return nullptr;
    }
    return {
        {"player", player->identity},
        {"status", player->playbackStatus},
        {"title", player->title},
        {"artists", player->artists},
        {"album", player->album},
        {"length_us", player->lengthUs},
    };
  }

  void bluetooth(const BluetoothState& state) { send("bluetooth", toJson(state)); }
  void network(const NetworkState& state) { send("network", toJson(state)); }
  void powerProfile(const PowerProfilesState& state) { send("power-profile", toJson(state)); }
  void media(const MprisService& service) { send("media", mediaJson(service)); }

} // namespace ipc_events
