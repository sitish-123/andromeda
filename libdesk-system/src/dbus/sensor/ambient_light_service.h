#pragma once

#include "core/timer_manager.h"

#include <chrono>
#include <functional>
#include <memory>

namespace sdbus {
  class IProxy;
}
class SystemBus;

// Ambient light in lux from iio-sensor-proxy (net.hadess.SensorProxy), sampled every 10 s, backing off to 60 s while
// the light stays steady.
// A sensor that reports only "vendor" units gives no readings: their scale differs per device.
class AmbientLightService {
public:
  using LuxCallback = std::function<void(double lux)>;

  AmbientLightService(SystemBus& bus, LuxCallback onLux);
  ~AmbientLightService();

  AmbientLightService(const AmbientLightService&) = delete;
  AmbientLightService& operator=(const AmbientLightService&) = delete;

  // Paused: no claim and no sampling; resuming samples at once.
  void setPaused(bool paused);

private:
  void sample();
  void refresh();
  void release();
  void releaseClaim();
  void onReading(double lux);

  std::unique_ptr<sdbus::IProxy> m_proxy;
  LuxCallback m_onLux;
  bool m_claimed = false;
  // The sensor reports lux, so a LightLevel change carries the reading itself.
  bool m_luxSensor = false;
  Timer m_readingWait;
  Timer m_nextSample;
  bool m_paused = false;
  double m_lastLux = -1.0;
  std::chrono::seconds m_interval{10};
  std::shared_ptr<int> m_lifetimeToken = std::make_shared<int>(0);
};
