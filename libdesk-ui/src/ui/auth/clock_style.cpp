#include "ui/auth/clock_style.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>

namespace {

  constexpr std::array<ClockStyle, 27> kStyles{{
      {.id = "classic", .family = "Inter", .file = "", .weight = 300},
      {.id = "bold", .family = "Inter", .file = "", .weight = 600, .tracking = -0.03F},
      {.id = "editorial", .family = "Fraunces", .file = "Fraunces-Variable.ttf", .weight = 300},
      {.id = "italic", .family = "Instrument Serif", .file = "InstrumentSerif-Italic.ttf", .weight = 400},
      {.id = "stacked", .family = "Space Grotesk", .file = "SpaceGrotesk-Variable.ttf", .weight = 600,
       .layout = ClockLayout::Stacked},
      {.id = "wide", .family = "Unbounded", .file = "Unbounded-Variable.ttf", .weight = 300, .tracking = 0.02F},
      {.id = "two-tone", .family = "Inter", .file = "", .weight = 600, .twoTone = true},
      {.id = "words", .family = "Fraunces", .file = "Fraunces-Variable.ttf", .weight = 400, .layout = ClockLayout::Words},
      {.id = "mono", .family = "JetBrains Mono", .file = "JetBrainsMono-Variable.ttf", .weight = 300},
      {.id = "condensed", .family = "Bebas Neue", .file = "BebasNeue-Regular.ttf", .weight = 400, .tracking = 0.02F},
      {.id = "live", .family = "Bricolage Grotesque", .file = "BricolageGrotesque-Variable.ttf", .weight = 200,
       .live = true},
      {.id = "dot-matrix", .family = "Doto", .file = "Doto-Variable.ttf", .weight = 600, .variations = "ROND=100"},
      {.id = "gradient", .family = "Inter", .file = "", .weight = 300, .gradient = true},
      {.id = "analog", .family = "Inter", .file = "", .weight = 300, .layout = ClockLayout::Analog},
      {.id = "flip", .family = "Inter", .file = "", .weight = 600, .layout = ClockLayout::Flip},
      {.id = "outline", .family = "Space Grotesk", .file = "SpaceGrotesk-Variable.ttf", .weight = 600, .stroke = 0.02F},
      {.id = "metropolitan", .family = "Bricolage Grotesque", .file = "BricolageGrotesque-Variable.ttf", .weight = 600,
       .breatheWidth = true},
      {.id = "stretch", .family = "Bricolage Grotesque", .file = "BricolageGrotesque-Variable.ttf", .weight = 800,
       .variations = "wdth=75", .stretch = 1.6F},
      {.id = "vertical-giant", .family = "Bricolage Grotesque", .file = "BricolageGrotesque-Variable.ttf",
       .weight = 800, .layout = ClockLayout::Stacked, .tracking = -0.04F, .variations = "wdth=75", .stretch = 1.25F},
      {.id = "morph", .family = "Inter", .file = "", .weight = 500, .morph = true},
      {.id = "typewriter", .family = "Recursive", .file = "Recursive-Variable.ttf", .weight = 400,
       .variations = "MONO=1,CASL=1", .caret = true},
      {.id = "neon", .family = "Space Grotesk", .file = "SpaceGrotesk-Variable.ttf", .weight = 500, .neon = true},
      {.id = "split", .family = "Inter", .file = "", .weight = 700, .splitOutline = true},
      {.id = "poster", .family = "Fraunces", .file = "Fraunces-Variable.ttf", .weight = 800,
       .layout = ClockLayout::Poster, .tracking = -0.02F, .variations = "SOFT=100"},
      {.id = "word-grid", .family = "JetBrains Mono", .file = "JetBrainsMono-Variable.ttf", .weight = 500,
       .layout = ClockLayout::WordGrid},
      {.id = "orbit", .family = "Inter", .file = "", .weight = 300, .layout = ClockLayout::Orbit},
      {.id = "casual", .family = "Recursive", .file = "Recursive-Variable.ttf", .weight = 700,
       .variations = "CASL=1,slnt=-12"},
  }};

  // English word clock (Qlocktwo grid): row text, then each word as {row, column, length}.
  constexpr std::array<std::string_view, 10> kGrid{
      "ITLISASTIME", "ACQUARTERDC", "TWENTYFIVEX", "HALFSTENFTO", "PASTERUNINE",
      "ONESIXTHREE", "FOURFIVETWO", "EIGHTELEVEN", "SEVENTWELVE", "TENSEOCLOCK",
  };
  struct GridWord {
    int row;
    int column;
    int length;
  };
  constexpr GridWord kItIs[2]{{0, 0, 2}, {0, 3, 2}};
  constexpr GridWord kQuarter{1, 2, 7};
  constexpr GridWord kTwenty{2, 0, 6};
  constexpr GridWord kFiveMin{2, 6, 4};
  constexpr GridWord kHalf{3, 0, 4};
  constexpr GridWord kTenMin{3, 5, 3};
  constexpr GridWord kTo{3, 9, 2};
  constexpr GridWord kPast{4, 0, 4};
  constexpr GridWord kOclock{9, 5, 6};
  // Hours 1-12.
  constexpr std::array<GridWord, 12> kHours{{
      {5, 0, 3}, {6, 8, 3}, {5, 6, 5}, {6, 0, 4}, {6, 4, 4}, {5, 3, 3},
      {8, 0, 5}, {7, 0, 5}, {4, 7, 4}, {9, 0, 3}, {7, 5, 6}, {8, 5, 6},
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

ClockFontAxes clockFontAxes(const ClockStyle& style) noexcept {
  const std::string_view file = style.file;
  if (file.starts_with("BricolageGrotesque")) {
    return {.weightMin = 200, .weightMax = 800, .widthMin = 75, .widthMax = 100};
  }
  if (file.starts_with("Doto")) {
    return {.weightMin = 100, .weightMax = 900, .roundMax = 100, .roundTag = "ROND"};
  }
  if (file.starts_with("Fraunces")) {
    return {.weightMin = 100, .weightMax = 900, .roundMax = 100, .roundTag = "SOFT"};
  }
  if (file.starts_with("JetBrainsMono")) {
    return {.weightMin = 100, .weightMax = 800};
  }
  if (file.starts_with("SpaceGrotesk")) {
    return {.weightMin = 300, .weightMax = 700};
  }
  if (file.starts_with("Unbounded")) {
    return {.weightMin = 200, .weightMax = 900};
  }
  if (file.starts_with("Recursive")) {
    return {.weightMin = 300, .weightMax = 1000, .slantMin = -15};
  }
  if (file.empty()) {
    return {.weightMin = 100, .weightMax = 900}; // Inter Variable
  }
  return {};
}

int clockWeightFor(const ClockStyle& style, const ClockAxisValues& values, int second) noexcept {
  const ClockFontAxes axes = clockFontAxes(style);
  if (values.weight <= 0 || !axes.weight() || style.live) {
    return clockWeightAt(style, second);
  }
  return static_cast<int>(std::clamp(static_cast<float>(values.weight), axes.weightMin, axes.weightMax));
}

std::string clockVariationsFor(const ClockStyle& style, const ClockAxisValues& values) {
  const ClockFontAxes axes = clockFontAxes(style);
  std::string out(style.variations);
  const auto add = [&out](std::string_view tag, float value) {
    out += std::format("{}{}={}", out.empty() ? "" : ",", tag, value);
  };
  if (values.width > 0 && axes.width()) {
    add("wdth", std::clamp(static_cast<float>(values.width), axes.widthMin, axes.widthMax));
  }
  if (values.round >= 0 && axes.round()) {
    add(axes.roundTag, std::clamp(static_cast<float>(values.round), 0.0F, axes.roundMax));
  }
  if (values.slant != 0 && axes.slant()) {
    add("slnt", std::clamp(static_cast<float>(-std::abs(values.slant)), axes.slantMin, 0.0F));
  }
  return out;
}

bool clockFillsSpace(std::string_view size) noexcept { return size == "fill"; }

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
  case ClockLayout::Analog:
  case ClockLayout::Orbit:
    // Two lines of room for the dial; the text stays for screen readers.
    return {hh, mm};
  case ClockLayout::Poster: {
    static constexpr std::array<std::string_view, 7> kDays{"Sunday", "Monday", "Tuesday", "Wednesday",
                                                           "Thursday", "Friday", "Saturday"};
    return {std::string(kDays[static_cast<std::size_t>(std::clamp(time.tm_wday, 0, 6))]),
            format.seconds ? std::format("{}:{}:{}", hh, mm, ss) : std::format("{}:{}", hh, mm)};
  }
  case ClockLayout::WordGrid:
    // Three lines of room for the 10 x 11 grid; the text stays for screen readers.
    return {hh, mm, std::string{}};
  case ClockLayout::Line:
  case ClockLayout::Flip:
    break;
  }
  if (style.stretch > 1.0F && style.layout == ClockLayout::Line) {
    // A stretched line fills a two-line block (auth_column scales it over both lines).
    return {format.seconds ? std::format("{}:{}:{}", hh, mm, ss) : std::format("{}:{}", hh, mm), std::string{}};
  }
  return {format.seconds ? std::format("{}:{}:{}", hh, mm, ss) : std::format("{}:{}", hh, mm)};
}

bool clockTicksSeconds(const ClockStyle& style, bool seconds, bool motion, const ClockAxisValues& axes) noexcept {
  return seconds || style.live || (motion && ((style.breatheWidth && axes.width <= 0) || style.caret));
}

float clockBreathWidth(const ClockStyle& style, int second) noexcept {
  const ClockFontAxes axes = clockFontAxes(style);
  const float t = 1.0F - std::abs(static_cast<float>(std::clamp(second, 0, 59)) - 30.0F) / 30.0F;
  return axes.widthMin + (axes.widthMax - axes.widthMin) * t;
}

std::span<const std::string_view> clockWordGrid() noexcept { return kGrid; }

std::vector<std::string> clockWordGridLit(const std::tm& time) {
  std::vector<std::string> lit(kGrid.size(), std::string(kGrid.front().size(), ' '));
  const auto light = [&](const GridWord& word) {
    for (int i = 0; i < word.length; ++i) {
      lit[static_cast<std::size_t>(word.row)][static_cast<std::size_t>(word.column + i)] =
          kGrid[static_cast<std::size_t>(word.row)][static_cast<std::size_t>(word.column + i)];
    }
  };
  for (const GridWord& word : kItIs) {
    light(word);
  }
  // Five-minute steps, rounded to the nearest; past the half the next hour counts down.
  const int step = ((time.tm_min + 2) / 5) % 12;
  int hour = time.tm_hour % 12 + (time.tm_min + 2 >= 60 ? 1 : 0);
  if (step > 6) {
    ++hour;
  }
  switch (step) {
  case 0:
    light(kOclock);
    break;
  case 1:
  case 11:
    light(kFiveMin);
    break;
  case 2:
  case 10:
    light(kTenMin);
    break;
  case 3:
  case 9:
    light(kQuarter);
    break;
  case 4:
  case 8:
    light(kTwenty);
    break;
  case 5:
  case 7:
    light(kTwenty);
    light(kFiveMin);
    break;
  case 6:
    light(kHalf);
    break;
  default:
    break;
  }
  if (step != 0) {
    light(step > 6 ? kTo : kPast);
  }
  light(kHours[static_cast<std::size_t>((hour % 12 + 11) % 12)]);
  return lit;
}

int clockWeightAt(const ClockStyle& style, int second) noexcept {
  return style.live ? 200 + (400 * std::clamp(second, 0, 59)) / 59 : style.weight;
}
