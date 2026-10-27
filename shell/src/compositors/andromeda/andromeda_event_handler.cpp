#include "compositors/andromeda/andromeda_event_handler.h"

#include "compositors/andromeda/andromeda_runtime.h"

namespace compositors::andromeda {

  AndromedaEventHandler::AndromedaEventHandler(AndromedaRuntime& runtime) : m_runtime(runtime) {
    m_runtime.registerEventHandler(this);
  }

  AndromedaEventHandler::~AndromedaEventHandler() { m_runtime.unregisterEventHandler(this); }

} // namespace compositors::andromeda
