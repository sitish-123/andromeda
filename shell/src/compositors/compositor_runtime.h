#pragma once

#include <memory>

namespace compositors {

  namespace andromeda {
    class AndromedaRuntime;
  } // namespace andromeda

  class CompositorRuntimeRegistry {
  public:
    CompositorRuntimeRegistry();
    ~CompositorRuntimeRegistry();

    CompositorRuntimeRegistry(const CompositorRuntimeRegistry&) = delete;
    CompositorRuntimeRegistry& operator=(const CompositorRuntimeRegistry&) = delete;

    [[nodiscard]] andromeda::AndromedaRuntime& andromeda() noexcept;
    [[nodiscard]] const andromeda::AndromedaRuntime& andromeda() const noexcept;

  private:
    std::unique_ptr<andromeda::AndromedaRuntime> m_andromeda;
  };

} // namespace compositors
