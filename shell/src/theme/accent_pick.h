#pragma once

#include "theme/color.h"

#include <cstdint>
#include <optional>
#include <span>

namespace noctalia::theme {

  // The accent for an RGBA8 image: the hue that holds the most saturated pixel weight, made legible under white
  // text. Empty when the image has almost no saturated colour (a grey or black-and-white picture).
  [[nodiscard]] std::optional<Color> pickImageAccent(std::span<const std::uint8_t> rgba);

  // `color` with its HSL lightness lowered until white text on it reaches 4.5:1.
  [[nodiscard]] Color accentForWhiteText(const Color& color);

} // namespace noctalia::theme
