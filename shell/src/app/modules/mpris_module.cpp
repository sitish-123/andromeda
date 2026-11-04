#include "app/modules/mpris_module.h"

#include "config/config_types.h"
#include "dbus/session_bus.h"

#include <exception>
#include <sdbus-c++/IObject.h>
#include <sdbus-c++/IProxy.h>

MprisModule::MprisModule(SessionBus& bus) : m_bus(bus) {}

MprisModule::~MprisModule() = default;

bool MprisModule::reads(const ConfigChangeSet& change) const { return change.shell; }

void MprisModule::start(const Config& config) {
  try {
    m_service = std::make_unique<MprisService>(m_bus);
    m_service->setBlacklist(config.shell.mpris.blacklist);
  } catch (const std::exception& e) {
    m_startError = e.what();
    m_service.reset();
  }
}

void MprisModule::stop() { m_service.reset(); }

void MprisModule::reload(const Config& config) {
  if (m_service != nullptr) {
    m_service->setBlacklist(config.shell.mpris.blacklist);
  }
}
