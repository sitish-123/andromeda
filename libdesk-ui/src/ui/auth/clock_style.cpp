#include "ui/auth/clock_style.h"

#include <algorithm>
#include <array>
#include <format>

namespace {

  constexpr std::array<ClockStyle, 12> kStyles{{
      {.id = "classic", .family = "Inter", .file = "", .weight = 300},
      {.id = "editorial", .family = "Fraunces", .file = "Fraunces-Variable.ttf", .weight = 300},
      {.id = "italic", .family = "Instrument Serif", .file = "InstrumentSerif-Italic.ttf", .weight = 400},
      {.id = "stacked", .family = "Space Grotesk", .file = "SpaceGrotesk-Variable.ttf", .weight = 600,
       .layout = ClockLayout::Stacked},
      {.id = "wide", .family = "Bricolage Grotesque", .file = "BricolageGrotesque-Variable.ttf", .weight = 200,
       .tracking = 0.08F},
      {.id = "two-tone", .family = "Inter", .file = "", .weight = 600, .twoTone = true},
      {.id = "words", .family = "Fraunces", .file = "Fraunces-Variable.ttf", .weight = 400, .layout = ClockLayout::Words},
      {.id = "mono", .family = "JetBrains Mono", .file = "JetBrainsMono-Variable.ttf", .weight = 300},
      {.id = "condensed", .family = "Bricolage Grotesque", .file = "BricolageGrotesque-Variable.ttf", .weight = 500,
       .variations = "wdth=75"},
      {.id = "live", .family = "Bricolage Grotesque", .file = "BricolageGrotesque-Variable.ttf", .weight = 200,
       .live = true},
      {.id = "dot-matrix", .family = "Doto", .file = "Doto-Variable.ttf", .weight = 600, .variations = "ROND=100"},
      {.id = "outline", .family = "Space Grotesk", .file = "SpaceGrotesk-Variable.ttf", .weight = 600, .stroke = 0.02F},
  }};

  constexpr std::array<std::string_view, 20> kOnes{
      "twelve", "one",     "two",     "three",     "four",     "five",    "six",
      "seven",  "eight",   "nine",    "ten",       "eleven",   "twelve",  "thirteen",
      "fourteen", "fifteen", "sixteen", "seventeen", "eighteen", "nineteen",
  };
  constexpr std::array<std::string_view, 6> kTens{"", "", "twenty", "thirty", "forty", "fifty"};

  std::string minuteWords(int m) {
    if (m == 0) {
      return "o'clock";
    }
    if (m < 10) {
      return "oh " + std::string(kOnes[static_cast<std::size_t>(m)]);
    }
    if (m < 20) {
      return std::string(kOnes[static_cast<std::size_t>(m)]);
    }
    std::string out(kTens[static_cast<std::size_t>(m / 10)]);
    if (m % 10 != 0) {
      out += "-" + std::string(kOnes[static_cast<std::size_t>(m % 10)]);
    }
    return out;
  }

} // namespace

std::span<const ClockStyle> clockStyles() noexcept { return kStyles; }

const ClockStyle& clockStyleById(std::string_view id) noexcept {
  for (const ClockStyle& style : kStyles) {
    if (style.id == id) {
      return style;
    }
  }
  return kStyles.front();
}

float clockSizeFor(std::string_view size) noexcept {
  if (size == "s") {
    return 64.0F;
  }
  if (size == "m") {
    return 80.0F;
  }
  if (size == "l") {
    return 96.0F;
  }
  if (size == "xl") {
    return 120.0F;
  }
  return 0.0F;
}

std::vector<std::string> clockLines(const ClockStyle& style, const std::tm& time, ClockFormat format) {
  const int hour = format.h24 ? time.tm_hour : (time.tm_hour % 12 == 0 ? 12 : time.tm_hour % 12);
  const std::string hh = format.h24 ? std::format("{:02}", hour) : std::to_string(hour);
  const std::string mm = std::format("{:02}", time.tm_min);
  const std::string ss = std::format("{:02}", time.tm_sec);
  switch (style.layout) {
  case ClockLayout::Stacked:
    return format.seconds ? std::vector<std::string>{hh, mm, ss} : std::vector<std::string>{hh, mm};
  case ClockLayout::Words:
    // Words read the 12 h clock; seconds do not apply.
    return {std::string(kOnes[static_cast<std::size_t>(time.tm_hour % 12)]), minuteWords(time.tm_min)};
  case ClockLayout::Line:
    break;
  }
  return {format.seconds ? std::format("{}:{}:{}", hh, mm, ss) : std::format("{}:{}", hh, mm)};
}

int clockWeightAt(const ClockStyle& style, int second) noexcept {
  return style.live ? 200 + (400 * std::clamp(second, 0, 59)) / 59 : style.weight;
}
