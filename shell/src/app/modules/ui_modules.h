#pragma once

#include "app/service_host.h"
#include "dbus/idle/screensaver_service.h"

#include <memory>
#include <string>

class SystemBus;

// org.freedesktop.ScreenSaver inhibit service. The service is null when it fails to start or finds no bus.
class ScreenSaverModule final : public ServiceModule {
public:
  explicit ScreenSaverModule(SystemBus* systemBus);
  ~ScreenSaverModule() override;

  [[nodiscard]] std::string_view name() const override { return "screensaver"; }
  void start(const Config& config) override;
  void stop() override;

  [[nodiscard]] ScreenSaverService* service() const noexcept { return m_service.get(); }
  // Reason the service failed to start; empty otherwise.
  [[nodiscard]] const std::string& startError() const noexcept { return m_startError; }

private:
  SystemBus* m_systemBus;
  std::unique_ptr<ScreenSaverService> m_service;
  std::string m_startError;
};
