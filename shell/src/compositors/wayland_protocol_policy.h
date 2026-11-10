#pragma once

#include "compositors/compositor_detect.h"

namespace wayland_protocol_policy {

  [[nodiscard]] constexpr bool shouldBindExtForeignToplevelList(compositors::CompositorKind compositor) noexcept {
    return compositor == compositors::CompositorKind::Andromeda;
  }

} // namespace wayland_protocol_policy
