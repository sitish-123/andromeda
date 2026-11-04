#pragma once

#include "app/service_host.h"
#include "dbus/power/power_profiles_service.h"
#include "dbus/upower/upower_service.h"

#include <functional>
#include <memory>

class SystemBus;

// power-profiles-daemon. The service is null when the daemon is missing.
class PowerProfilesModule final : public ServiceModule {
public:
  struct Hooks {
    // Runs once, when the first snapshot arrives, before any change.
    std::function<void(PowerProfilesService&)> started;
    PowerProfilesService::ChangeCallback changed;
  };

  PowerProfilesModule(SystemBus& bus, Hooks hooks);

  [[nodiscard]] std::string_view name() const override { return "power-profiles"; }
  void start(const Config& config) override;
  void stop() override { m_service.reset(); }

  [[nodiscard]] PowerProfilesService* service() const noexcept { return m_service.get(); }

private:
  SystemBus& m_bus;
  Hooks m_hooks;
  std::unique_ptr<PowerProfilesService> m_service;
};

// UPower batteries and power source. The service is null when UPower is missing.
class UPowerModule final : public ServiceModule {
public:
  struct Hooks {
    // Runs once, when the first snapshot arrives, before any change.
    std::function<void(UPowerService&)> started;
    UPowerService::ChangeCallback changed;
    // Runs when the [battery] section changed.
    std::function<void()> batteryChanged;
  };

  UPowerModule(SystemBus& bus, Hooks hooks);

  [[nodiscard]] std::string_view name() const override { return "upower"; }
  // The power source picks the active power profile as soon as UPower starts.
  [[nodiscard]] std::vector<std::string_view> needs() const override { return {"power-profiles"}; }
  [[nodiscard]] bool reads(const ConfigChangeSet& change) const override;
  void start(const Config& config) override;
  void stop() override { m_service.reset(); }
  void reload(const Config& config) override;

  [[nodiscard]] UPowerService* service() const noexcept { return m_service.get(); }

private:
  SystemBus& m_bus;
  Hooks m_hooks;
  std::unique_ptr<UPowerService> m_service;
};
