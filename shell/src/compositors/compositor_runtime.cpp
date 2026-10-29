#include "compositors/compositor_runtime.h"

#include "compositors/andromeda/andromeda_runtime.h"
#include "compositors/hyprland/hyprland_runtime.h"
#include "compositors/mango/mango_runtime.h"
#include "compositors/niri/niri_runtime.h"
#include "compositors/sway/sway_runtime.h"

namespace compositors {

  CompositorRuntimeRegistry::CompositorRuntimeRegistry()
      : m_hyprland(std::make_unique<hyprland::HyprlandRuntime>()), m_mango(std::make_unique<mango::MangoRuntime>()),
        m_niri(std::make_unique<niri::NiriRuntime>()), m_sway(std::make_unique<sway::SwayRuntime>()),
        m_andromeda(std::make_unique<andromeda::AndromedaRuntime>()) {}

  CompositorRuntimeRegistry::~CompositorRuntimeRegistry() = default;

  hyprland::HyprlandRuntime& CompositorRuntimeRegistry::hyprland() noexcept { return *m_hyprland; }

  const hyprland::HyprlandRuntime& CompositorRuntimeRegistry::hyprland() const noexcept { return *m_hyprland; }

  mango::MangoRuntime& CompositorRuntimeRegistry::mango() noexcept { return *m_mango; }

  const mango::MangoRuntime& CompositorRuntimeRegistry::mango() const noexcept { return *m_mango; }

  niri::NiriRuntime& CompositorRuntimeRegistry::niri() noexcept { return *m_niri; }

  const niri::NiriRuntime& CompositorRuntimeRegistry::niri() const noexcept { return *m_niri; }

  sway::SwayRuntime& CompositorRuntimeRegistry::sway() noexcept { return *m_sway; }

  const sway::SwayRuntime& CompositorRuntimeRegistry::sway() const noexcept { return *m_sway; }

  andromeda::AndromedaRuntime& CompositorRuntimeRegistry::andromeda() noexcept { return *m_andromeda; }

  const andromeda::AndromedaRuntime& CompositorRuntimeRegistry::andromeda() const noexcept { return *m_andromeda; }

} // namespace compositors
