#include "dbus/timedate/timedate_service.h"

#include "core/log.h"
#include "dbus/system_bus.h"

#include <map>
#include <optional>
#include <sdbus-c++/sdbus-c++.h>
#include <string>
#include <utility>

namespace {

  constexpr Logger kLog("timedate1");
  const sdbus::ServiceName kBusName{"org.freedesktop.timedate1"};
  const sdbus::ObjectPath kObjectPath{"/org/freedesktop/timedate1"};
  constexpr auto kInterface = "org.freedesktop.timedate1";
  constexpr auto kPropertiesInterface = "org.freedesktop.DBus.Properties";

  template <typename T> T getPropertyOr(sdbus::IProxy& proxy, std::string_view name, T fallback) {
    try {
      return proxy.getProperty(name).onInterface(kInterface).get<T>();
    } catch (const sdbus::Error&) {
      return fallback;
    }
  }

} // namespace

TimeDateService::TimeDateService(SystemBus& bus, ChangeCallback onChange)
    : m_bus(bus), m_onChange(std::move(onChange)) {
  try {
    m_proxy = sdbus::createProxy(m_bus.connection(), kBusName, kObjectPath);
    m_proxy->uponSignal("PropertiesChanged")
        .onInterface(kPropertiesInterface)
        .call([this](
                  const std::string& interfaceName, const std::map<std::string, sdbus::Variant>& /*changed*/,
                  const std::vector<std::string>& /*invalidated*/
              ) {
          if (interfaceName == kInterface) {
            refreshProperties();
            if (m_onChange) {
              m_onChange();
            }
          }
        });
    refreshProperties();
    m_proxy->callMethodAsync("ListTimezones")
        .onInterface(kInterface)
        .uponReplyInvoke([this](std::optional<sdbus::Error> error, std::vector<std::string> zones) {
          if (error.has_value()) {
            kLog.warn("ListTimezones failed: {}", error->what());
            return;
          }
          m_timezones = std::move(zones);
          if (m_onChange) {
            m_onChange();
          }
        });
    m_ready = true;
  } catch (const sdbus::Error& e) {
    kLog.warn("timedate1 unavailable: {}", e.what());
  }
}

TimeDateService::~TimeDateService() = default;

void TimeDateService::refreshProperties() {
  if (m_proxy == nullptr) {
    return;
  }
  m_timezone = getPropertyOr<std::string>(*m_proxy, "Timezone", {});
  m_ntp = getPropertyOr<bool>(*m_proxy, "NTP", false);
  m_localRtc = getPropertyOr<bool>(*m_proxy, "LocalRTC", false);
  m_ntpSynchronized = getPropertyOr<bool>(*m_proxy, "NTPSynchronized", false);
  m_timeUsec = getPropertyOr<std::int64_t>(*m_proxy, "TimeUSec", 0);
}

void TimeDateService::setTimezone(const std::string& zone) {
  if (m_proxy == nullptr) {
    return;
  }
  m_lastError.clear();
  m_proxy->callMethodAsync("SetTimezone")
      .onInterface(kInterface)
      .withArguments(zone, true)
      .uponReplyInvoke([this](std::optional<sdbus::Error> error) {
        if (error.has_value()) {
          m_lastError = std::string(error->what());
          kLog.warn("SetTimezone failed: {}", error->what());
        } else {
          refreshProperties();
        }
        if (m_onChange) {
          m_onChange();
        }
      });
}

void TimeDateService::setNtp(bool useNtp) {
  if (m_proxy == nullptr) {
    return;
  }
  m_lastError.clear();
  m_proxy->callMethodAsync("SetNTP")
      .onInterface(kInterface)
      .withArguments(useNtp, true)
      .uponReplyInvoke([this](std::optional<sdbus::Error> error) {
        if (error.has_value()) {
          m_lastError = std::string(error->what());
          kLog.warn("SetNTP failed: {}", error->what());
        } else {
          refreshProperties();
        }
        if (m_onChange) {
          m_onChange();
        }
      });
}

void TimeDateService::setTime(std::int64_t usecUtc) {
  if (m_proxy == nullptr) {
    return;
  }
  m_lastError.clear();
  m_proxy->callMethodAsync("SetTime")
      .onInterface(kInterface)
      .withArguments(usecUtc, false, true)
      .uponReplyInvoke([this](std::optional<sdbus::Error> error) {
        if (error.has_value()) {
          m_lastError = std::string(error->what());
          kLog.warn("SetTime failed: {}", error->what());
        } else {
          refreshProperties();
        }
        if (m_onChange) {
          m_onChange();
        }
      });
}

void TimeDateService::setLocalRtc(bool localRtc, bool fixSystem) {
  if (m_proxy == nullptr) {
    return;
  }
  m_lastError.clear();
  m_proxy->callMethodAsync("SetLocalRTC")
      .onInterface(kInterface)
      .withArguments(localRtc, fixSystem, true)
      .uponReplyInvoke([this](std::optional<sdbus::Error> error) {
        if (error.has_value()) {
          m_lastError = std::string(error->what());
          kLog.warn("SetLocalRTC failed: {}", error->what());
        } else {
          refreshProperties();
        }
        if (m_onChange) {
          m_onChange();
        }
      });
}
