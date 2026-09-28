#pragma once

#include <functional>
#include <string>
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
  };

  // Phones paired over Umbriel Link and Quick Share visibility, both served by umbriel-linkd.
  void addSettingsDevices(Flex& content, const SettingsDevicesContext& ctx);

} // namespace settings
