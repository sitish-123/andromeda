#include "dbus/link/link_phone_state.h"

#include "core/log.h"
#include "i18n/i18n.h"

#include <algorithm>

namespace {

  constexpr Logger kLog("link");

} // namespace

const LinkStatus* LinkPhoneState::status(const std::string& deviceId) const {
  const auto it = m_status.find(deviceId);
  return it != m_status.end() ? &it->second : nullptr;
}

bool LinkPhoneState::granted(const std::string& deviceId, std::string_view feature) const {
  const auto it = m_grants.find(deviceId);
  return it != m_grants.end() && std::ranges::contains(it->second, feature);
}

std::uint32_t LinkPhoneState::notificationCount(const std::string& deviceId) const {
  const auto it = m_notificationCounts.find(deviceId);
  return it != m_notificationCounts.end() ? it->second : 0;
}

void LinkPhoneState::applyPhoneProperties(const std::map<std::string, sdbus::Variant>& properties) {
  if (const auto it = properties.find("Devices"); it != properties.end()) {
    try {
      m_devices.clear();
      for (const auto& device : it->second.get<std::vector<sdbus::Struct<std::string, std::string, bool>>>()) {
        m_devices.push_back({.id = device.get<0>(), .name = device.get<1>(), .connected = device.get<2>()});
      }
    } catch (const sdbus::Error& e) {
      kLog.warn("malformed Devices: {}", e.what());
    }
  }
  if (const auto it = properties.find("DeviceStatus"); it != properties.end()) {
    try {
      m_status.clear();
      for (const auto& [id, status] :
           it->second.get<std::map<std::string, sdbus::Struct<std::uint32_t, bool, std::string, std::int32_t>>>()) {
        m_status[id] = LinkStatus{
            .battery = status.get<0>(),
            .charging = status.get<1>(),
            .network = status.get<2>(),
            .signal = status.get<3>(),
        };
      }
    } catch (const sdbus::Error& e) {
      kLog.warn("malformed DeviceStatus: {}", e.what());
    }
  }
  if (const auto it = properties.find("Grants"); it != properties.end()) {
    try {
      m_grants = it->second.get<std::map<std::string, std::vector<std::string>>>();
    } catch (const sdbus::Error& e) {
      kLog.warn("malformed Grants: {}", e.what());
    }
  }
  if (const auto it = properties.find("NotificationCounts"); it != properties.end()) {
    try {
      m_notificationCounts = it->second.get<std::map<std::string, std::uint32_t>>();
    } catch (const sdbus::Error& e) {
      kLog.warn("malformed NotificationCounts: {}", e.what());
    }
  }
}

void LinkPhoneState::clearPhoneState() {
  m_devices.clear();
  m_status.clear();
  m_grants.clear();
  m_notificationCounts.clear();
}

void LinkPhoneState::applyCall(
    const std::string& deviceId, const std::string& state, const std::string& number, const std::string& name,
    const std::string& app
) {
  if (state == "ringing" || state == "active") {
    const std::string who =
        !name.empty() ? name : (!number.empty() ? number : i18n::tr("notifications.internal.link-call-unknown"));
    const bool wasActive = m_call.has_value() && m_call->deviceId == deviceId && m_call->since.has_value();
    m_call = LinkCallStatus{
        .deviceId = deviceId,
        .caller = who,
        .device = app.empty() ? deviceName(deviceId) : app + " · " + deviceName(deviceId),
        .ringing = state == "ringing",
        .since = state == "ringing" ? std::nullopt
            : wasActive             ? m_call->since
                                    : std::optional(std::chrono::steady_clock::now()),
    };
  } else if (m_call.has_value() && m_call->deviceId == deviceId) {
    m_call.reset();
  }
}

std::string LinkPhoneState::deviceName(const std::string& deviceId) const {
  const auto device = std::ranges::find(m_devices, deviceId, &LinkDevice::id);
  return device != m_devices.end() ? device->name : deviceId;
}
