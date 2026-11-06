#pragma once

#include <algorithm>
#include <array>

namespace bar_detail {

  // x, y, width, height in bar surface coordinates.
  using IslandRect = std::array<float, 4>;

  // Distance a side island moves toward its bar end while it steps aside: its own thickness.
  [[nodiscard]] inline float
  islandAsideShift(const IslandRect& rest, float progress, bool towardStart, bool isVertical) {
    const float travel = isVertical ? rest[2] : rest[3];
    return (towardStart ? -travel : travel) * std::clamp(progress, 0.0F, 1.0F);
  }

  // A side island at overview step-aside `progress` (0 in place, 1 aside): it slides toward its bar end and shrinks
  // about its centre to nothing, so its glass pill (which the compositor cannot fade) ends empty.
  [[nodiscard]] inline IslandRect
  islandAsideRect(const IslandRect& rest, float progress, bool towardStart, bool isVertical) {
    const float scale = 1.0F - std::clamp(progress, 0.0F, 1.0F);
    const float width = rest[2] * scale;
    const float height = rest[3] * scale;
    const float shift = islandAsideShift(rest, progress, towardStart, isVertical);
    const float x = rest[0] + (rest[2] - width) * 0.5F + (isVertical ? 0.0F : shift);
    const float y = rest[1] + (rest[3] - height) * 0.5F + (isVertical ? shift : 0.0F);
    return {x, y, width, height};
  }

} // namespace bar_detail
