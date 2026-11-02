#pragma once

#include <functional>
#include <string>

class BluetoothService;
class Flex;

namespace settings {

  struct SettingsBluetoothContext {
    float scale = 1.0F;
    BluetoothService* bluetooth = nullptr;
    // "bt:<alias>" with the device path after a tab while a device's detail page is open, else empty.
    std::string detail;
    std::function<void(std::string)> openDetail;
    std::function<void()> back;
  };

  // Bluetooth page (direction A, .agent/DESIGN.md): hero card, My Devices, Nearby Devices; or one device's details.
  void addSettingsBluetooth(Flex& content, const SettingsBluetoothContext& ctx);

} // namespace settings
