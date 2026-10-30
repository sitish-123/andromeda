#include "core/log.h"
#include "core/process/process.h"
#include "dbus/link/link_service.h"

#include <sdbus-c++/IProxy.h>

namespace {

  constexpr Logger kLog("link");
  constexpr auto kLinkInterface = "org.andromeda.Link1";

} // namespace

void LinkService::applyDeviceState(const std::map<std::string, sdbus::Variant>& properties) {
  if (const auto it = properties.find("DeviceInfo"); it != properties.end()) {
    try {
      m_info.clear();
      for (const auto& [id, fields] : it->second.get<std::map<std::string, std::map<std::string, sdbus::Variant>>>()) {
        LinkDeviceInfo& info = m_info[id];
        if (const auto seen = fields.find("last_seen"); seen != fields.end()) {
          info.lastSeen = seen->second.get<std::int64_t>();
        }
        if (const auto transport = fields.find("transport"); transport != fields.end()) {
          info.transport = transport->second.get<std::string>();
        }
      }
    } catch (const sdbus::Error& e) {
      kLog.warn("malformed DeviceInfo: {}", e.what());
    }
  }
  if (const auto it = properties.find("DeviceControls"); it != properties.end()) {
    try {
      m_controls.clear();
      using Controls = sdbus::Struct<std::int32_t, std::string, std::int32_t, std::int32_t>;
      for (const auto& [id, controls] : it->second.get<std::map<std::string, Controls>>()) {
        m_controls[id] = LinkControls{
            .mediaVolume = controls.get<0>(),
            .ringer = controls.get<1>(),
            .dnd = controls.get<2>(),
        };
      }
    } catch (const sdbus::Error& e) {
      kLog.warn("malformed DeviceControls: {}", e.what());
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

const LinkDeviceInfo* LinkService::info(const std::string& deviceId) const {
  const auto it = m_info.find(deviceId);
  return it != m_info.end() ? &it->second : nullptr;
}

std::uint32_t LinkService::notificationCount(const std::string& deviceId) const {
  const auto it = m_notificationCounts.find(deviceId);
  return it != m_notificationCounts.end() ? it->second : 0;
}

const LinkControls* LinkService::controls(const std::string& deviceId) const {
  const auto it = m_controls.find(deviceId);
  return it != m_controls.end() ? &it->second : nullptr;
}

void LinkService::sendSystemCommand(const std::string& deviceId, const std::string& command, std::uint32_t value) {
  m_link->callMethodAsync("SendSystemCommand")
      .onInterface(kLinkInterface)
      .withArguments(deviceId, command, value)
      .uponReplyInvoke([command](std::optional<sdbus::Error> error) {
        if (error.has_value()) {
          kLog.warn("SendSystemCommand {} failed: {}", command, error->what());
        }
      });
}

void LinkService::openApp() {
  if (!process::runAsync(std::vector<std::string>{"andromeda-link"})) {
    kLog.warn("could not start andromeda-link");
  }
}
