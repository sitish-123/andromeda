#pragma once

#include "ui/palette.h"

#include <cstdint>

// Hairline stroke of the glass material (--stroke) in logical pixels.
inline constexpr float kIslandStrokeWidth = 0.5F;

// Island outline width: a configured width wins; else the hairline in light only, since the dark stroke
// (white 8 %) reads as a highlight on a dark pill (user, 2026-11-02).
[[nodiscard]] inline float islandStrokeWidth(float configured) noexcept {
  return configured > 0.0F ? configured : (isResolvedLightTheme() ? kIslandStrokeWidth : 0.0F);
}

// Row: the island sits in a top or bottom bar row; Left and Right: in a vertical bar's lane, centred on the output.
enum class IslandLane : std::uint8_t { Row, Left, Right };

// Where the always-on center island may sit on one output: the bar row between the side islands.
struct CenterIslandSpace {
  std::int32_t edgeMargin = 0; // screen edge to the island row
  float height = 0.0F;         // height of the side islands
  float width = 0.0F;          // free center width, already minus the gap to each side island
  bool bottom = false;
  bool end = false; // a full-width bar: banners sit at the trailing screen edge, as on macOS and Windows
  bool restless = false; // no island bar: no rest pill, the island shows only for a banner or live activity
  IslandLane lane = IslandLane::Row;
  float scale = 1.0F;             // the bar's content scale (ui_scale included): the island's layout scale
  float opacity = 1.0F;           // follows the bar's auto-hide fade
  float backgroundOpacity = 1.0F; // the bar's island material
  ColorSpec border;
  float borderWidth = 0.0F;
};
