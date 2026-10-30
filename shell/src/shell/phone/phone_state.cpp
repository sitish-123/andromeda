#include "shell/phone/phone_state.h"

#include "dbus/link/link_service.h"
#include "dbus/upower/upower_service.h"
#include "i18n/i18n.h"
#include "time/time_format.h"

#include <algorithm>
#include <chrono>
#include <format>

namespace phone {

  const LinkDevice* primary(const LinkService& link) {
    const auto& devices = link.devices();
    if (devices.empty()) {
      return nullptr;
    }
    const auto connected = std::ranges::find_if(devices, &LinkDevice::connected);
    return connected != devices.end() ? &*connected : &devices.front();
  }

  std::string connectionText(const LinkService& link, const LinkDevice& device) {
    const LinkDeviceInfo* info = link.info(device.id);
    if (device.connected) {
      if (info != nullptr && (info->transport == "wifi" || info->transport == "bluetooth")) {
        return i18n::tr("bar.widgets.phone.transport-" + info->transport);
      }
      return i18n::tr("bar.widgets.phone.connected");
    }
    if (info == nullptr || info->lastSeen <= 0) {
      return i18n::tr("bar.widgets.phone.not-connected");
    }
    const auto seen = std::chrono::system_clock::time_point(std::chrono::seconds(info->lastSeen));
    return i18n::tr("bar.widgets.phone.last-seen", "time", formatTimeAgo(seen));
  }

  std::string batteryText(const LinkStatus& status) {
    std::string text = std::format("{}%", status.battery);
    if (status.charging) {
      text += ", " + i18n::tr("bar.widgets.phone.charging");
    }
    return text;
  }

  std::string glyph(const LinkDevice& device, const LinkStatus* status) {
    if (status != nullptr) {
      return batteryGlyphName(status->battery, status->charging ? BatteryState::Charging : BatteryState::Discharging);
    }
    return device.connected ? "device-mobile" : "device-mobile-off";
  }

  std::uint32_t nextRinger(std::string_view ringer) {
    if (ringer == "normal") {
      return 1;
    }
    if (ringer == "vibrate") {
      return 0;
    }
    return 2;
  }

  std::string ringerGlyph(std::string_view ringer) {
    if (ringer == "vibrate") {
      return "device-mobile-vibration";
    }
    return ringer == "silent" ? "bell-off" : "bell";
  }

} // namespace phone
