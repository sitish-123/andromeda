#pragma once

#include <functional>
#include <string>

class Flex;
class INetworkService;

namespace settings {

  struct SettingsWifiContext {
    float scale = 1.0F;
    INetworkService* network = nullptr;
    // "wifi:<ssid>" while a network's detail page is open, else empty.
    std::string detail;
    std::string joinSsid;
    // Reads the typed text at click time: typing does not rebuild the row.
    std::function<std::string()> password;
    std::function<void(std::string)> openDetail;
    std::function<void(std::string)> setJoinSsid;
    std::function<void(std::string)> setPassword;
    std::function<void()> back;
  };

  // Wi-Fi page (direction A, .agent/DESIGN.md): hero card, Known networks, Other networks; or one network's details.
  void addSettingsWifi(Flex& content, const SettingsWifiContext& ctx);

} // namespace settings
