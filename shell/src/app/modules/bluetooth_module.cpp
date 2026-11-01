#include "app/modules/bluetooth_module.h"

#include "config/config_types.h"
#include "core/log.h"

#include <exception>

namespace {

  constexpr Logger kLog("app");

} // namespace

BluetoothModule::BluetoothModule(SystemBus& bus, const UPowerModule& upower, Hooks hooks)
    : m_bus(bus), m_upower(upower), m_hooks(std::move(hooks)) {}

bool BluetoothModule::reads(const ConfigChangeSet& change) const { return change.bluetooth; }

void BluetoothModule::start(const Config& config) {
  try {
    m_service = std::make_unique<BluetoothService>(m_bus, m_upower.service());
    m_service->setConnectWhenEnabled(config.bluetooth.connectWhenEnabled);
    m_service->setConnectionNotifications(config.bluetooth.connectionNotifications);
    m_service->setStateCallback(m_hooks.stateChanged);
    m_service->setDevicesCallback(m_hooks.devicesChanged);
    m_hooks.started(*m_service);
    kLog.info("bluetooth service active");
  } catch (const std::exception& e) {
    kLog.warn("bluetooth service disabled: {}", e.what());
    m_service.reset();
  }

  if (m_service != nullptr) {
    try {
      m_agent = std::make_unique<BluetoothAgent>(m_bus);
      m_agent->setRequestCallback(m_hooks.pairingRequest);
    } catch (const std::exception& e) {
      kLog.warn("bluetooth agent disabled: {}", e.what());
      m_agent.reset();
    }
  }
}

void BluetoothModule::stop() {
  m_agent.reset();
  m_service.reset();
}

void BluetoothModule::reload(const Config& config) {
  if (m_service != nullptr) {
    m_service->setConnectWhenEnabled(config.bluetooth.connectWhenEnabled);
    m_service->setConnectionNotifications(config.bluetooth.connectionNotifications);
  }
}
