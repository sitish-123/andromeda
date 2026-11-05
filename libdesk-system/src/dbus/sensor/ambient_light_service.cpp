#include "dbus/sensor/ambient_light_service.h"

#include "core/log.h"
#include "dbus/system_bus.h"

#include <algorithm>
#include <chrono>
#include <cmath>
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
  constexpr auto kSampleInterval = std::chrono::seconds(10);
  constexpr auto kMaxSampleInterval = std::chrono::seconds(60);
  constexpr auto kReadingWait = std::chrono::seconds(3);
} // namespace

AmbientLightService::AmbientLightService(SystemBus& bus, LuxCallback onLux) : m_onLux(std::move(onLux)) {
  m_proxy = sdbus::createProxy(bus.connection(), kBusName, kPath);
  m_proxy->uponSignal("PropertiesChanged")
      .onInterface(kPropertiesInterface)
      .call([this](const std::string& interfaceName, const VariantMap& changed, const std::vector<std::string>&) {
        if (interfaceName != kInterface || !m_claimed) {
          return;
        }
        // iio-sensor-proxy sends the unit with every reading.
        if (const auto unit = changed.find("LightLevelUnit"); unit != changed.end()) {
          m_luxSensor = unit->second.get<std::string>() == "lux";
        }
        if (const auto level = changed.find("LightLevel"); level != changed.end() && m_luxSensor) {
          onReading(level->second.get<double>());
          release();
        }
      });
  sample();
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

// The proxy polls the sensor only while a client holds the claim, so each sample claims it, takes the first fresh
// reading (or the current value if none comes), and releases it until the next sample.
void AmbientLightService::sample() {
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
    m_readingWait.start(kReadingWait, [this] { refresh(); });
  });
}

void AmbientLightService::releaseClaim() {
  m_readingWait.stop();
  if (m_claimed) {
    m_claimed = false;
    try {
      m_proxy->callMethod("ReleaseLight").onInterface(kInterface).dontExpectReply();
    } catch (const sdbus::Error& e) {
      kLog.debug("release light: {}", e.what());
    }
  }
}

void AmbientLightService::release() {
  releaseClaim();
  if (!m_paused) {
    m_nextSample.start(m_interval, [this] { sample(); });
  }
}

// A steady reading (within 15 % or 5 lux) doubles the interval up to 60 s; a change returns it to 10 s.
void AmbientLightService::onReading(double lux) {
  const bool steady = m_lastLux >= 0.0 && std::abs(lux - m_lastLux) <= std::max(5.0, 0.15 * m_lastLux);
  m_interval = steady ? std::min(m_interval * 2, kMaxSampleInterval) : kSampleInterval;
  m_lastLux = lux;
  m_onLux(lux);
}

void AmbientLightService::setPaused(bool paused) {
  if (paused == m_paused) {
    return;
  }
  m_paused = paused;
  m_nextSample.stop();
  if (paused) {
    releaseClaim();
    return;
  }
  m_interval = kSampleInterval;
  sample();
}

void AmbientLightService::refresh() {
  const std::weak_ptr<int> alive = m_lifetimeToken;
  m_proxy->callMethodAsync("GetAll")
      .onInterface(kPropertiesInterface)
      .withArguments(std::string{kInterface})
      .uponReplyInvoke([this, alive](std::optional<sdbus::Error> err, const VariantMap& properties) {
        if (alive.expired()) {
          return;
        }
        const auto has = properties.find("HasAmbientLight");
        const auto unit = properties.find("LightLevelUnit");
        const auto level = properties.find("LightLevel");
        m_luxSensor = !err.has_value() && has != properties.end() && has->second.get<bool>() && unit != properties.end()
            && unit->second.get<std::string>() == "lux" && level != properties.end();
        if (m_luxSensor) {
          onReading(level->second.get<double>());
        }
        release();
      });
}
