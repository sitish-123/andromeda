#pragma once

#include "ui/palette.h"

#include <cstdint>

// Where the always-on center island may sit on one output: the bar row between the side islands.
struct CenterIslandSpace {
  std::int32_t edgeMargin = 0; // screen edge to the island row
  float height = 0.0F;         // height of the side islands
  float width = 0.0F;          // free center width, already minus the gap to each side island
  bool bottom = false;
  float opacity = 1.0F;           // follows the bar's auto-hide fade
  float backgroundOpacity = 1.0F; // the bar's island material
  ColorSpec border;
  float borderWidth = 0.0F;
};
