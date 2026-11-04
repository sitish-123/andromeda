#include "dbus/sensor/ambient_light_service.h"

#include "core/log.h"
#include "dbus/system_bus.h"

#include <map>
#include <optional>
#include <sdbus-c++/IProxy.h>
#include <sdbus-c++/Types.h>
#include <string>
#include <vector>

namespace {
  constexpr Logger kLog("ambient-light");
  const sdbus::ServiceName kBusName{"net.hadess.SensorProxy"};
  const sdbus::ObjectPath kPath{"/net/hadess/SensorProxy"};
  constexpr auto kInterface = "net.hadess.SensorProxy";
  constexpr auto kPropertiesInterface = "org.freedesktop.DBus.Properties";
  using VariantMap = std::map<std::string, sdbus::Variant>;
} // namespace

AmbientLightService::AmbientLightService(SystemBus& bus, LuxCallback onLux) : m_onLux(std::move(onLux)) {
  m_proxy = sdbus::createProxy(bus.connection(), kBusName, kPath);
  m_proxy->uponSignal("PropertiesChanged")
      .onInterface(kPropertiesInterface)
      .call([this](const std::string& interfaceName, const VariantMap& changed, const std::vector<std::string>&) {
        if (interfaceName == kInterface && (changed.contains("LightLevel") || changed.contains("HasAmbientLight"))) {
          refresh();
        }
      });
  const std::weak_ptr<int> alive = m_lifetimeToken;
  m_proxy->callMethodAsync("ClaimLight").onInterface(kInterface).uponReplyInvoke([this, alive](std::optional<sdbus::Error> err) {
    if (alive.expired()) {
      return;
    }
    if (err.has_value()) {
      kLog.info("no ambient light sensor: {}", err->what());
      return;
    }
    m_claimed = true;
    refresh();
  });
}

AmbientLightService::~AmbientLightService() {
  m_lifetimeToken.reset();
  if (m_claimed) {
    try {
      m_proxy->callMethod("ReleaseLight").onInterface(kInterface).dontExpectReply();
    } catch (const sdbus::Error& e) {
      kLog.debug("release light: {}", e.what());
    }
  }
}

void AmbientLightService::refresh() {
  const std::weak_ptr<int> alive = m_lifetimeToken;
  m_proxy->callMethodAsync("GetAll")
      .onInterface(kPropertiesInterface)
      .withArguments(std::string{kInterface})
      .uponReplyInvoke([this, alive](std::optional<sdbus::Error> err, const VariantMap& properties) {
        if (alive.expired() || err.has_value()) {
          return;
        }
        const auto has = properties.find("HasAmbientLight");
        const auto unit = properties.find("LightLevelUnit");
        const auto level = properties.find("LightLevel");
        if (has == properties.end() || !has->second.get<bool>() || unit == properties.end()
            || unit->second.get<std::string>() != "lux" || level == properties.end()) {
          return;
        }
        m_onLux(level->second.get<double>());
      });
}
