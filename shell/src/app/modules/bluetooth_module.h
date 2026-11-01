#pragma once

#include "app/modules/power_modules.h"
#include "app/service_host.h"
#include "dbus/bluetooth/bluetooth_agent.h"
#include "dbus/bluetooth/bluetooth_service.h"

#include <functional>
#include <memory>

class SystemBus;

// BlueZ adapter, devices and the pairing agent. The service is null when BlueZ is missing.
class BluetoothModule final : public ServiceModule {
public:
  struct Hooks {
    // Runs once the service exists and its callbacks are set.
    std::function<void(BluetoothService&)> started;
    BluetoothService::StateCallback stateChanged;
    BluetoothService::DevicesCallback devicesChanged;
    BluetoothAgent::RequestCallback pairingRequest;
  };

  BluetoothModule(SystemBus& bus, const UPowerModule& upower, Hooks hooks);

  [[nodiscard]] std::string_view name() const override { return "bluetooth"; }
  // The service keeps a raw pointer to the UPower service.
  [[nodiscard]] std::vector<std::string_view> needs() const override { return {"upower"}; }
  [[nodiscard]] bool reads(const ConfigChangeSet& change) const override;
  void start(const Config& config) override;
  void stop() override;
  void reload(const Config& config) override;

  [[nodiscard]] BluetoothService* service() const noexcept { return m_service.get(); }
  [[nodiscard]] BluetoothAgent* agent() const noexcept { return m_agent.get(); }

private:
  SystemBus& m_bus;
  const UPowerModule& m_upower;
  Hooks m_hooks;
  std::unique_ptr<BluetoothService> m_service;
  std::unique_ptr<BluetoothAgent> m_agent;
};
