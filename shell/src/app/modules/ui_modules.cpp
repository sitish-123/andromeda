#include "app/modules/ui_modules.h"

#include <exception>

ScreenSaverModule::ScreenSaverModule(SystemBus* systemBus) : m_systemBus(systemBus) {}

ScreenSaverModule::~ScreenSaverModule() = default;

void ScreenSaverModule::start(const Config& /*config*/) {
  try {
    m_service = std::make_unique<ScreenSaverService>(m_systemBus);
    if (!m_service->active()) {
      m_service.reset();
    }
  } catch (const std::exception& e) {
    m_startError = e.what();
    m_service.reset();
  }
}

void ScreenSaverModule::stop() { m_service.reset(); }
