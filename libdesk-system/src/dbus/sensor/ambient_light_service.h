#pragma once

#include <functional>
#include <memory>

namespace sdbus {
  class IProxy;
}
class SystemBus;

// Ambient light in lux from iio-sensor-proxy (net.hadess.SensorProxy). It claims the light sensor while alive.
// A sensor that reports only "vendor" units gives no readings: their scale differs per device.
class AmbientLightService {
public:
  using LuxCallback = std::function<void(double lux)>;

  AmbientLightService(SystemBus& bus, LuxCallback onLux);
  ~AmbientLightService();

  AmbientLightService(const AmbientLightService&) = delete;
  AmbientLightService& operator=(const AmbientLightService&) = delete;

private:
  void refresh();

  std::unique_ptr<sdbus::IProxy> m_proxy;
  LuxCallback m_onLux;
  bool m_claimed = false;
  std::shared_ptr<int> m_lifetimeToken = std::make_shared<int>(0);
};
