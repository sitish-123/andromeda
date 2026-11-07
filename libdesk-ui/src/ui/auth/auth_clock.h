#pragma once

#include "ui/auth/auth_column.h"
#include "ui/auth/clock_style.h"

#include <cstdint>
#include <string>
#include <string_view>

// Font family of a clock style; registers its bundled font file on first use.
[[nodiscard]] std::string clockFontFamily(const ClockStyle& clock);

struct AuthClockOptions {
  std::string_view style; // clock style id (clock_style.h)
  bool h24 = true;
  bool seconds = false;
  ClockAxisValues axes;
  // lockscreen.clock_color: white | accent | wallpaper (gradientFrom) | gradient (every style); else white.
  std::string_view color;
  // lockscreen.clock_material: solid | tint | glass (the compositor draws glass in the digit shapes, clockGlassMask).
  std::string_view material;
  // lockscreen.clock_motion and not Reduce Motion: morph, breathing width and the caret blink run.
  bool motion = true;
};

// Clock lines, font, effects and date of a clock style at the current local time (lock screen, its editor, greeter).
void fillAuthClock(
    AuthColumn::State& state, const AuthClockOptions& options, std::uint32_t gradientFrom, std::uint32_t gradientTo
);
