#include "compositors/compositor_runtime.h"

#include "compositors/andromeda/andromeda_runtime.h"

namespace compositors {

  CompositorRuntimeRegistry::CompositorRuntimeRegistry()
      : m_andromeda(std::make_unique<andromeda::AndromedaRuntime>()) {}

  CompositorRuntimeRegistry::~CompositorRuntimeRegistry() = default;

  andromeda::AndromedaRuntime& CompositorRuntimeRegistry::andromeda() noexcept { return *m_andromeda; }

  const andromeda::AndromedaRuntime& CompositorRuntimeRegistry::andromeda() const noexcept { return *m_andromeda; }

} // namespace compositors
