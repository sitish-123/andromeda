#pragma once

#include "render/core/render_styles.h"
#include "ui/shadow_style.h"

#include <cstdint>

namespace ui::surface_shadow {

  constexpr std::int32_t kBlurRadius = 12;

  struct Bleed {
    std::int32_t left = 0;
    std::int32_t right = 0;
    std::int32_t up = 0;
    std::int32_t down = 0;
  };

  struct Shape {
    CornerShapes corners{};
    RectInsets logicalInset{};
    Radii radius;
  };

  [[nodiscard]] bool enabled(bool componentShadow, const ShadowStyle& shadow) noexcept;
  [[nodiscard]] Bleed bleed(bool componentShadow, const ShadowStyle& shadow) noexcept;
  [[nodiscard]] RoundedRectStyle
  style(const ShadowStyle& shadow, float backgroundOpacity, const Shape& shape) noexcept;
  [[nodiscard]] bool
  sameSurfaceMetrics(const ShadowStyle& previous, const ShadowStyle& next) noexcept;

} // namespace ui::surface_shadow
