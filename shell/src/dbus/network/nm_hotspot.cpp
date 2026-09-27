#include "dbus/network/nm_hotspot.h"

#include "core/log.h"
#include "dbus/system_bus.h"

#include <map>
#include <random>
#include <sdbus-c++/sdbus-c++.h>
#include <unistd.h>
#include <vector>

namespace {

  constexpr Logger kLog("hotspot");

  const sdbus::ServiceName kNmBusName{"org.freedesktop.NetworkManager"};
  const sdbus::ObjectPath kNmPath{"/org/freedesktop/NetworkManager"};
  const sdbus::ObjectPath kSettingsPath{"/org/freedesktop/NetworkManager/Settings"};
  constexpr auto kNmInterface = "org.freedesktop.NetworkManager";
  constexpr auto kSettingsInterface = "org.freedesktop.NetworkManager.Settings";
  constexpr auto kConnectionInterface = "org.freedesktop.NetworkManager.Settings.Connection";
  constexpr auto kActiveInterface = "org.freedesktop.NetworkManager.Connection.Active";
  constexpr auto kDeviceInterface = "org.freedesktop.NetworkManager.Device";
  constexpr auto kWirelessInterface = "org.freedesktop.NetworkManager.Device.Wireless";
  constexpr std::uint32_t kDeviceTypeWifi = 2;
  constexpr std::uint32_t kWifiCapAp = 0x40;

  using Settings = std::map<std::string, std::map<std::string, sdbus::Variant>>;

  std::unique_ptr<sdbus::IProxy> proxy(SystemBus& bus, const std::string& path) {
    return sdbus::createProxy(bus.connection(), kNmBusName, sdbus::ObjectPath{path});
  }

  std::string hostname() {
    char name[256] = {};
    return ::gethostname(name, sizeof(name) - 1) == 0 && name[0] != '\0' ? std::string(name) : std::string("Noctalia");
  }

  std::string randomKey() {
    constexpr std::string_view kAlphabet = "abcdefghijkmnpqrstuvwxyz23456789";
    std::random_device random;
    std::uniform_int_distribution<std::size_t> pick(0, kAlphabet.size() - 1);
    std::string key;
    for (int i = 0; i < 12; ++i) {
      key += kAlphabet[pick(random)];
    }
    return key;
  }

  Settings newProfile() {
    const std::string ssid = hostname();
    Settings settings;
    settings["connection"]["id"] = sdbus::Variant{std::string("Hotspot")};
    settings["connection"]["type"] = sdbus::Variant{std::string("802-11-wireless")};
    settings["connection"]["autoconnect"] = sdbus::Variant{false};
    settings["802-11-wireless"]["ssid"] = sdbus::Variant{std::vector<std::uint8_t>(ssid.begin(), ssid.end())};
    settings["802-11-wireless"]["mode"] = sdbus::Variant{std::string("ap")};
    settings["802-11-wireless-security"]["key-mgmt"] = sdbus::Variant{std::string("wpa-psk")};
    settings["802-11-wireless-security"]["proto"] = sdbus::Variant{std::vector<std::string>{"rsn"}};
    settings["802-11-wireless-security"]["psk"] = sdbus::Variant{randomKey()};
    settings["ipv4"]["method"] = sdbus::Variant{std::string("shared")};
    settings["ipv6"]["method"] = sdbus::Variant{std::string("ignore")};
    return settings;
  }

} // namespace

NmHotspot::NmHotspot(SystemBus& bus) : m_bus(bus), m_nm(sdbus::createProxy(bus.connection(), kNmBusName, kNmPath)) {}

NmHotspot::~NmHotspot() = default;

std::string NmHotspot::findProfile() const {
  std::vector<sdbus::ObjectPath> paths;
  proxy(m_bus, kSettingsPath)->callMethod("ListConnections").onInterface(kSettingsInterface).storeResultsTo(paths);
  for (const auto& path : paths) {
    Settings settings;
    proxy(m_bus, path)->callMethod("GetSettings").onInterface(kConnectionInterface).storeResultsTo(settings);
    const auto wireless = settings.find("802-11-wireless");
    if (wireless == settings.end()) {
      continue;
    }
    const auto mode = wireless->second.find("mode");
    if (mode != wireless->second.end() && mode->second.get<std::string>() == "ap") {
      return path;
    }
  }
  return {};
}

std::string NmHotspot::findApDevice() const {
  std::vector<sdbus::ObjectPath> devices;
  m_nm->callMethod("GetDevices").onInterface(kNmInterface).storeResultsTo(devices);
  for (const auto& path : devices) {
    const auto device = proxy(m_bus, path);
    if (device->getProperty("DeviceType").onInterface(kDeviceInterface).get<std::uint32_t>() == kDeviceTypeWifi
        && (device->getProperty("WirelessCapabilities").onInterface(kWirelessInterface).get<std::uint32_t>()
            & kWifiCapAp)
            != 0) {
      return path;
    }
  }
  return {};
}

std::string NmHotspot::activeFor(const std::string& profile) const {
  if (profile.empty()) {
    return {};
  }
  const auto active =
      m_nm->getProperty("ActiveConnections").onInterface(kNmInterface).get<std::vector<sdbus::ObjectPath>>();
  for (const auto& path : active) {
    if (proxy(m_bus, path)->getProperty("Connection").onInterface(kActiveInterface).get<sdbus::ObjectPath>()
        == profile) {
      return path;
    }
  }
  return {};
}

bool NmHotspot::active() const {
  try {
    return !activeFor(findProfile()).empty();
  } catch (const sdbus::Error& e) {
    kLog.debug("hotspot state unavailable: {}", e.what());
    return false;
  }
}

std::string NmHotspot::setEnabled(bool enabled) {
  try {
    const std::string profile = findProfile();
    if (!enabled) {
      const std::string activePath = activeFor(profile);
      if (!activePath.empty()) {
        m_nm->callMethod("DeactivateConnection").onInterface(kNmInterface).withArguments(sdbus::ObjectPath{activePath});
      }
      return {};
    }

    const std::string device = findApDevice();
    if (device.empty()) {
      return "no Wi-Fi device can host a hotspot";
    }
    // Asynchronous: activation can wait on polkit.
    const auto onReply = [](std::optional<sdbus::Error> error, auto&&...) {
      if (error.has_value()) {
        kLog.warn("hotspot activation failed: {}", error->what());
      }
    };
    if (!profile.empty()) {
      m_nm->callMethodAsync("ActivateConnection")
          .onInterface(kNmInterface)
          .withArguments(sdbus::ObjectPath{profile}, sdbus::ObjectPath{device}, sdbus::ObjectPath{"/"})
          .uponReplyInvoke([onReply](std::optional<sdbus::Error> error, sdbus::ObjectPath /*active*/) {
            onReply(std::move(error));
          });
    } else {
      m_nm->callMethodAsync("AddAndActivateConnection")
          .onInterface(kNmInterface)
          .withArguments(newProfile(), sdbus::ObjectPath{device}, sdbus::ObjectPath{"/"})
          .uponReplyInvoke([onReply](
                               std::optional<sdbus::Error> error, sdbus::ObjectPath /*profile*/,
                               sdbus::ObjectPath /*active*/
                           ) { onReply(std::move(error)); });
    }
    return {};
  } catch (const sdbus::Error& e) {
    return e.what();
  }
}
