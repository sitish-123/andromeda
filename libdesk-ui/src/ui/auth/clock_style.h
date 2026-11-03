#pragma once

#include <ctime>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Rulebook 9.15 clock styles: presets of font, weight, layout and effect for the lock screen clock. The fonts are the
// bundled OFL files under assets/fonts/lock (Inter is the shell's own font).
enum class ClockLayout { Line, Stacked, Words };

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
  // Two-tone: the minutes in accent_text.
  bool twoTone = false;
};

// Every built-in style; the first is the default.
[[nodiscard]] std::span<const ClockStyle> clockStyles() noexcept;
// Unknown ids give the default style.
[[nodiscard]] const ClockStyle& clockStyleById(std::string_view id) noexcept;
// lockscreen.clock.size: s 64, m 80, l 96, xl 120; anything else = 0 (the layout's own size).
[[nodiscard]] float clockSizeFor(std::string_view size) noexcept;

struct ClockFormat {
  bool h24 = true;
  bool seconds = false;
};

// The clock text, one entry per line: Line = "10:42", Stacked = "10" / "42", Words = "ten" / "forty-two".
// 12 h drops the leading zero and has no AM/PM (the date line carries the day).
[[nodiscard]] std::vector<std::string> clockLines(const ClockStyle& style, const std::tm& time, ClockFormat format);
