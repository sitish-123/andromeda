#pragma once

#include <ctime>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Rulebook 9.15 clock styles: presets of font, weight, layout and effect for the lock screen clock. The fonts are the
// bundled OFL files under assets/fonts/lock (Inter is the shell's own font).
// Poster = weekday over the time; WordGrid = a lit-letter grid ("it is ten past ten"); Orbit = an hour hand and a
// planet on an orbit ring for the minutes.
enum class ClockLayout { Line, Stacked, Words, Analog, Flip, Poster, WordGrid, Orbit };

struct ClockStyle {
  std::string_view id;
  // Font family as fontconfig names it once the file is registered.
  std::string_view family;
  // File under assets/fonts/lock; empty = a font the shell already has (Inter).
  std::string_view file;
  int weight = 300;
  ClockLayout layout = ClockLayout::Line;
  // Letter spacing in em.
  float tracking = 0.0F;
  // Outline style: stroke width in em; 0 = filled.
  float stroke = 0.0F;
  // Extra font axes, "wdth=75".
  std::string_view variations;
  // Gradient: the ink runs from the palette primary_fixed to tertiary_fixed.
  bool gradient = false;
  // Live: the weight follows the seconds, light at :00 to bold at :59.
  bool live = false;
  // Two-tone: the minutes in accent_text.
  bool twoTone = false;
  // Vertical scale of the lines (Stretch, Vertical giant); 1 = none.
  float stretch = 1.0F;
  // Metropolitan: the width axis breathes with the seconds (narrow at :00 and :59, wide at :30). Motion.
  bool breatheWidth = false;
  // Morph: a new minute unfolds from a squashed, faded line. Motion.
  bool morph = false;
  // Typewriter: a caret after the time that blinks each second. Motion.
  bool caret = false;
  // Neon: accent ink with a wide faint outline around each glyph as the glow.
  bool neon = false;
  // Split: the minutes as outlines next to filled hours.
  bool splitOutline = false;
  // 7-segment: the unlit segments ("88:88") drawn faint under the time.
  bool ghost = false;
};

// The variable axes of a style's font; a range with max <= min = the font has no such axis. Round is ROND or SOFT.
struct ClockFontAxes {
  float weightMin = 0.0F;
  float weightMax = 0.0F;
  float widthMin = 0.0F;
  float widthMax = 0.0F;
  float roundMax = 0.0F;
  std::string_view roundTag;
  float slantMin = 0.0F;
  [[nodiscard]] bool weight() const noexcept { return weightMax > weightMin; }
  [[nodiscard]] bool width() const noexcept { return widthMax > widthMin; }
  [[nodiscard]] bool round() const noexcept { return roundMax > 0.0F && !roundTag.empty(); }
  [[nodiscard]] bool slant() const noexcept { return slantMin < 0.0F; }
};
[[nodiscard]] ClockFontAxes clockFontAxes(const ClockStyle& style) noexcept;

// lockscreen.clock_weight / clock_width / clock_round / clock_slant: 0 (round -1) = the style's own value.
struct ClockAxisValues {
  int weight = 0;
  int width = 0;
  int round = -1;
  int slant = 0;
};
// The weight to draw: the override clamped to the font's range, else the style's (live) weight.
[[nodiscard]] int clockWeightFor(const ClockStyle& style, const ClockAxisValues& values, int second) noexcept;
// Pango axis string: the style's own axes, then the overrides the font has ("wdth=75,ROND=100").
[[nodiscard]] std::string clockVariationsFor(const ClockStyle& style, const ClockAxisValues& values);

// Every built-in style; the first is the default.
[[nodiscard]] std::span<const ClockStyle> clockStyles() noexcept;
// Unknown ids give the default style.
[[nodiscard]] const ClockStyle& clockStyleById(std::string_view id) noexcept;
// lockscreen.clock.size: s 64, m 80, l 96, xl 120; anything else = 0 (the layout's own size). "fill" is placed by
// the layout (clockFillsSpace).
[[nodiscard]] float clockSizeFor(std::string_view size) noexcept;
[[nodiscard]] bool clockFillsSpace(std::string_view size) noexcept;
// The clock needs a layout each second (seconds shown, live weight, breathing width unless `axes` fixes the width,
// blinking caret).
[[nodiscard]] bool clockTicksSeconds(const ClockStyle& style, bool seconds, bool motion, const ClockAxisValues& axes) noexcept;
// Metropolitan's width axis at `second`, in the font's width range.
[[nodiscard]] float clockBreathWidth(const ClockStyle& style, int second) noexcept;
// Word grid: 10 rows of 11 letters, and the lit mask for `time` (same size, ' ' = unlit).
[[nodiscard]] std::span<const std::string_view> clockWordGrid() noexcept;
[[nodiscard]] std::vector<std::string> clockWordGridLit(const std::tm& time);
// The style's weight; a live style's weight at `second` (0-59), 200 to 600.
[[nodiscard]] int clockWeightAt(const ClockStyle& style, int second) noexcept;

struct ClockFormat {
  bool h24 = true;
  bool seconds = false;
};

// The clock text, one entry per line: Line = "10:42", Stacked = "10" / "42", Words = "ten" / "forty-two".
// 12 h drops the leading zero and has no AM/PM (the date line carries the day).
[[nodiscard]] std::vector<std::string> clockLines(const ClockStyle& style, const std::tm& time, ClockFormat format);
