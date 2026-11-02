#pragma once

#include "ui/palette.h"

#include <cstdint>

// Top edge highlight of the glass material (the reference --edge): white, 0.549 in light, 0.122 in dark.
[[nodiscard]] inline Color islandEdgeColor() noexcept {
  return Color{.r = 1.0F, .g = 1.0F, .b = 1.0F, .a = isResolvedLightTheme() ? 0.549F : 0.122F};
}

// Hairline stroke of the glass material (--stroke) in logical pixels.
inline constexpr float kIslandStrokeWidth = 0.5F;

// Where the always-on center island may sit on one output: the bar row between the side islands.
struct CenterIslandSpace {
  std::int32_t edgeMargin = 0; // screen edge to the island row
  float height = 0.0F;         // height of the side islands
  float width = 0.0F;          // free center width, already minus the gap to each side island
  bool bottom = false;
  bool restless = false; // left or right bar: no rest pill, the island shows only for a banner or live activity
  float opacity = 1.0F;           // follows the bar's auto-hide fade
  float backgroundOpacity = 1.0F; // the bar's island material
  ColorSpec border;
  float borderWidth = 0.0F;
};
