#include "app/modules/power_modules.h"

#include "config/config_types.h"
#include "core/log.h"

#include <exception>

namespace {

  constexpr Logger kLog("app");

} // namespace

PowerProfilesModule::PowerProfilesModule(SystemBus& bus, Hooks hooks) : m_bus(bus), m_hooks(std::move(hooks)) {}

void PowerProfilesModule::start(const Config& /*config*/) {
  try {
    m_service = std::make_unique<PowerProfilesService>(m_bus);
    m_service->setChangeCallback(m_hooks.changed);
    m_hooks.started(*m_service);
    if (m_service->hasStateSnapshot() && !m_service->activeProfile().empty()) {
      kLog.info("power profiles active profile: {}", m_service->activeProfile());
    } else if (!m_service->hasStateSnapshot()) {
      kLog.info("power profiles service active (state loading asynchronously)");
    } else {
      kLog.info("power profiles service active");
    }
  } catch (const std::exception& e) {
    kLog.warn("power profiles disabled: {}", e.what());
    m_service.reset();
  }
}

UPowerModule::UPowerModule(SystemBus& bus, Hooks hooks) : m_bus(bus), m_hooks(std::move(hooks)) {}

bool UPowerModule::reads(const ConfigChangeSet& change) const { return change.battery; }

void UPowerModule::start(const Config& /*config*/) {
  try {
    m_service = std::make_unique<UPowerService>(m_bus);
    m_service->setChangeCallback([this](const UPowerChange& change) {
      if (change.origin == UPowerService::ChangeOrigin::Initial) {
        m_hooks.started(*m_service);
        return;
      }
      m_hooks.changed(change);
    });
  } catch (const std::exception& e) {
    kLog.warn("upower disabled: {}", e.what());
    m_service.reset();
  }
}

void UPowerModule::reload(const Config& /*config*/) {
  if (m_service != nullptr) {
    m_hooks.batteryChanged();
  }
}
