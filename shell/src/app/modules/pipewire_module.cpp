#include "app/modules/pipewire_module.h"

#include <exception>

void PipeWireModule::start(const Config& /*config*/) {
  try {
    m_service = std::make_unique<PipeWireService>();
  } catch (const std::exception& e) {
    m_startError = e.what();
    m_service.reset();
  }
}

void PipeWireModule::stop() { m_service.reset(); }
