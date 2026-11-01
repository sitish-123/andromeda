#include "app/modules/network_module.h"

#include "core/log.h"
#include "dbus/network/iwd_service.h"
#include "dbus/network/network_manager_service.h"
#include "dbus/network/wpa_supplicant_service.h"

#include <exception>

namespace {

  constexpr Logger kLog("app");

} // namespace

NetworkModule::NetworkModule(SystemBus& bus, Hooks hooks) : m_bus(bus), m_hooks(std::move(hooks)) {}

void NetworkModule::start(const Config& /*config*/) {
  // NetworkManager is preferred, and its backend follows the bus name when NetworkManager starts later
  // or restarts. When it is not running at startup, a standalone wpa_supplicant or iwd takes over if
  // one is; otherwise the NetworkManager backend waits for it to appear.
  try {
    m_service = std::make_unique<NetworkManagerService>(m_bus);
  } catch (const std::exception& e) {
    kLog.warn("NetworkManager backend disabled: {}", e.what());
  }
  if (m_service == nullptr || !m_service->available()) {
    try {
      m_service = std::make_unique<WpaSupplicantService>(m_bus);
      kLog.info("network service active (wpa_supplicant)");
    } catch (const std::exception& e) {
      kLog.warn("wpa_supplicant unavailable ({}), trying iwd", e.what());
      try {
        m_service = std::make_unique<IwdService>(m_bus);
        kLog.info("network service active (iwd)");
      } catch (const std::exception& e2) {
        kLog.warn("iwd unavailable ({})", e2.what());
      }
    }
  } else {
    kLog.info("network service active");
  }

  if (m_service != nullptr) {
    m_service->setChangeCallback(m_hooks.changed);
    m_hooks.started(*m_service);
  }

  try {
    m_secretAgent = std::make_unique<NetworkSecretAgent>(m_bus);
  } catch (const std::exception& e) {
    kLog.warn("network secret agent disabled: {}", e.what());
    m_secretAgent.reset();
  }

  if (auto* iwdService = dynamic_cast<IwdService*>(m_service.get())) {
    try {
      m_iwdSecretAgent = std::make_unique<IwdSecretAgent>(m_bus);
      iwdService->setSecretAgent(m_iwdSecretAgent.get());
    } catch (const std::exception& e) {
      kLog.warn("iwd secret agent disabled: {}", e.what());
      m_iwdSecretAgent.reset();
    }
  }
}

void NetworkModule::stop() {
  m_iwdSecretAgent.reset();
  m_secretAgent.reset();
  m_service.reset();
}
