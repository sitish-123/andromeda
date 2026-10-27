#pragma once

#include <functional>
#include <string>
#include <utility>
#include <vector>

class Flex;

namespace settings {

  struct SettingsDevicePhone {
    std::string id;
    std::string name;
    bool connected = false;
  };

  struct SettingsDevicesContext {
    float scale = 1.0F;
    bool linkAvailable = false;
    std::vector<SettingsDevicePhone> phones;
    bool quickShareAvailable = false;
    bool quickShareVisible = false;
    std::string quickShareName;
    std::string downloads;
    std::function<void()> pair;
    std::function<void(std::string)> unpair;
    std::function<void(bool)> setQuickShareVisible;
    // (name, command line) that phones with the commands grant may run.
    std::vector<std::pair<std::string, std::string>> commands;
    std::function<void(std::vector<std::pair<std::string, std::string>>)> setCommands;
  };

  // Phones paired over Andromeda Link and Quick Share visibility, both served by andromeda-linkd.
  void addSettingsDevices(Flex& content, const SettingsDevicesContext& ctx);

} // namespace settings
