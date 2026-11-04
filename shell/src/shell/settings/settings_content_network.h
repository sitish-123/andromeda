#pragma once

#include <functional>
#include <string>

class Flex;
class INetworkService;

namespace settings {

  struct SettingsNetworkContext {
    float scale = 1.0F;
    INetworkService* network = nullptr;
    std::function<void(std::string)> openPage;
  };

  // Network page (direction A, .agent/DESIGN.md): one row per service (Wi-Fi, Ethernet, mobile data), VPN rows with
  // toggles, and the current connection's address.
  void addSettingsNetwork(Flex& content, const SettingsNetworkContext& ctx);

} // namespace settings
