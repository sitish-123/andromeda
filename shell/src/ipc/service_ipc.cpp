#include "ipc/service_ipc.h"

#include "dbus/bluetooth/bluetooth_service.h"
#include "dbus/network/inetwork_service.h"
#include "dbus/power/power_profiles_service.h"
#include "ipc/ipc_service.h"
#include "util/string_utils.h"

#include <algorithm>
#include <optional>
#include <string>

namespace {

  std::optional<std::string> rejectArgs(std::string_view command, const std::string& args) {
    if (StringUtils::trim(args).empty()) {
      return std::nullopt;
    }
    return "error: " + std::string(command) + " takes no arguments\n";
  }

} // namespace

void registerPowerProfilesIpc(
    IpcService& ipc, PowerProfilesService& service, const std::function<void(std::string_view)>& stateFeedback
) {
  ipc.bind(noctalia::cli::msg::powerSet, [&service, stateFeedback](const std::string& args) -> std::string {
    const std::string profile = StringUtils::trim(args);
    if (profile.empty()) {
      return "error: profile required (power-set <profile>); typical values: performance, balanced, "
             "power-saver\n";
    }
    const auto& available = service.profiles();
    if (!available.empty() && !std::ranges::contains(available, profile)) {
      std::string suffix = "; available:";
      for (const auto& availableProfile : available) {
        suffix.push_back(' ');
        suffix += availableProfile;
      }
      suffix.push_back('\n');
      return "error: unknown profile \"" + profile + "\"" + suffix;
    }

    const std::string previous = service.activeProfile();
    if (!service.setActiveProfile(profile)) {
      return "error: failed to set power profile\n";
    }
    if (stateFeedback && previous != service.activeProfile() && !service.activeProfile().empty()) {
      stateFeedback(service.activeProfile());
    }
    return "ok\n";
  });
  ipc.bindCycle(noctalia::cli::msg::powerCycle, [&service, stateFeedback](const std::string& args) -> std::string {
    const std::string direction = StringUtils::trim(args);
    int step = 1;
    if (direction == "prev") {
      step = -1;
    } else if (!direction.empty() && direction != "next") {
      return "error: power-cycle takes next or prev\n";
    }
    const std::string previous = service.activeProfile();
    if (!service.cycleActiveProfile(step)) {
      if (service.profiles().empty() && service.activeProfile().empty()) {
        return "error: could not cycle power profile (UPower profile list not loaded)\n";
      }
      return "error: could not cycle power profile (set failed)\n";
    }
    if (stateFeedback && previous != service.activeProfile() && !service.activeProfile().empty()) {
      stateFeedback(service.activeProfile());
    }
    return "ok\n";
  });
}

void registerBluetoothIpc(IpcService& ipc, BluetoothService& service, const std::function<void(bool)>& stateFeedback) {
  auto setBluetooth = [&service, stateFeedback](bool enabled) -> std::string {
    if (!service.hasStateSnapshot()) {
      return "error: bluetooth state unavailable\n";
    }
    if (!service.state().adapterPresent) {
      return "error: bluetooth adapter unavailable\n";
    }
    if (service.state().powered == enabled) {
      return "ok\n";
    }
    service.setPowered(enabled);
    if (stateFeedback) {
      stateFeedback(enabled);
    }
    return "ok\n";
  };

  ipc.bind(noctalia::cli::msg::bluetoothEnable, [setBluetooth](const std::string& args) -> std::string {
    if (auto err = rejectArgs("bluetooth-enable", args); err.has_value()) {
      return *err;
    }
    return setBluetooth(true);
  });

  ipc.bind(noctalia::cli::msg::bluetoothDisable, [setBluetooth](const std::string& args) -> std::string {
    if (auto err = rejectArgs("bluetooth-disable", args); err.has_value()) {
      return *err;
    }
    return setBluetooth(false);
  });

  ipc.bind(noctalia::cli::msg::bluetoothToggle, [&service, setBluetooth](const std::string& args) -> std::string {
    if (auto err = rejectArgs("bluetooth-toggle", args); err.has_value()) {
      return *err;
    }
    if (!service.hasStateSnapshot()) {
      return "error: bluetooth state unavailable\n";
    }
    return setBluetooth(!service.state().powered);
  });

  ipc.bind(
      noctalia::cli::msg::bluetoothStatus,
      [&service](const std::string& args) -> std::string {
        if (auto err = rejectArgs("bluetooth-status", args); err.has_value()) {
          return *err;
        }
        if (!service.hasStateSnapshot()) {
          return "error: bluetooth state unavailable\n";
        }
        return service.state().powered ? "on\n" : "off\n";
      },
      IpcService::HandlerOptions{.actionEditorVisibility = IpcService::ActionEditorVisibility::Hidden}
  );
}

void registerNetworkIpc(IpcService& ipc, INetworkService& service, const std::function<void(bool)>& wirelessFeedback) {
  auto setWifi = [&service, wirelessFeedback](bool enabled) -> std::string {
    if (!service.hasStateSnapshot()) {
      return "error: network state unavailable\n";
    }
    if (service.state().wirelessEnabled == enabled) {
      return "ok\n";
    }
    service.setWirelessEnabled(enabled);
    if (wirelessFeedback) {
      wirelessFeedback(enabled);
    }
    return "ok\n";
  };

  ipc.bind(noctalia::cli::msg::wifiEnable, [setWifi](const std::string& args) -> std::string {
    if (auto err = rejectArgs("wifi-enable", args); err.has_value()) {
      return *err;
    }
    return setWifi(true);
  });

  ipc.bind(noctalia::cli::msg::wifiDisable, [setWifi](const std::string& args) -> std::string {
    if (auto err = rejectArgs("wifi-disable", args); err.has_value()) {
      return *err;
    }
    return setWifi(false);
  });

  ipc.bind(noctalia::cli::msg::wifiToggle, [&service, setWifi](const std::string& args) -> std::string {
    if (auto err = rejectArgs("wifi-toggle", args); err.has_value()) {
      return *err;
    }
    if (!service.hasStateSnapshot()) {
      return "error: network state unavailable\n";
    }
    return setWifi(!service.state().wirelessEnabled);
  });

  ipc.bind(
      noctalia::cli::msg::wifiStatus,
      [&service](const std::string& args) -> std::string {
        if (auto err = rejectArgs("wifi-status", args); err.has_value()) {
          return *err;
        }
        if (!service.hasStateSnapshot()) {
          return "error: network state unavailable\n";
        }
        return service.state().wirelessEnabled ? "on\n" : "off\n";
      },
      IpcService::HandlerOptions{.actionEditorVisibility = IpcService::ActionEditorVisibility::Hidden}
  );

  ipc.bind(noctalia::cli::msg::networkToggle, [&service, setWifi](const std::string& args) -> std::string {
    if (auto err = rejectArgs("network-toggle", args); err.has_value()) {
      return *err;
    }
    if (!service.hasStateSnapshot()) {
      return "error: network state unavailable\n";
    }
    const NetworkState& s = service.state();
    // Drop whatever is up, otherwise bring back whichever transport can come up.
    if (s.kind == NetworkConnectivity::Wireless && (s.connected || s.resolving)) {
      return setWifi(false);
    }
    if (s.kind == NetworkConnectivity::Wired && (s.connected || s.resolving)) {
      service.disconnect();
      return "ok\n";
    }
    if (s.kind == NetworkConnectivity::Cellular && (s.connected || s.resolving)) {
      if (!service.deactivateCellularConnection()) {
        return "error: failed to disconnect the cellular connection\n";
      }
      return "ok\n";
    }
    if (!s.wirelessEnabled) {
      return setWifi(true);
    }
    if (service.canActivateWiredConnection()) {
      if (!service.activateWiredConnection()) {
        return "error: failed to activate the wired connection\n";
      }
      return "ok\n";
    }
    if (service.canActivateCellularConnection()) {
      if (!service.activateCellularConnection()) {
        return "error: failed to activate the cellular connection\n";
      }
      return "ok\n";
    }
    return "error: nothing to toggle (Wi-Fi is on and no wired or cellular connection is available)\n";
  });
}
