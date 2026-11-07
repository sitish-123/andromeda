#include "ui/auth/auth_clock.h"

#include "core/files/resource_paths.h"
#include "render/text/font_registry.h"
#include "time/time_format.h"

#include <ctime>
#include <format>
#include <unordered_map>

// A clock style's bundled font is registered on first use, not at start, so the start budget pays nothing.
std::string clockFontFamily(const ClockStyle& clock) {
  if (clock.file.empty()) {
    return std::string(clock.family);
  }
  // Each bundled font file registers once per process.
  static std::unordered_map<std::string, std::string> families;
  const std::string key(clock.file);
  if (const auto it = families.find(key); it != families.end()) {
    return it->second;
  }
  std::string family = text::registerFontFile(paths::assetPath("fonts/lock/" + key));
  if (family.empty()) {
    family = std::string(clock.family);
  }
  families.emplace(key, family);
  return family;
}

namespace {
  // accent_text on the dark lock backdrop (auth_column.cpp kAccentText).
  constexpr std::uint32_t kAccentTextInk = 0x8C85FFFFU;
} // namespace

void fillAuthClock(
    AuthColumn::State& state, const AuthClockOptions& options, std::uint32_t gradientFrom, std::uint32_t gradientTo
) {
  state.time = formatLocalTime("%H:%M");
  const ClockStyle& clock = clockStyleById(options.style);
  const std::time_t now = std::time(nullptr);
  std::tm local{};
  localtime_r(&now, &local);
  state.clockLines = clockLines(clock, local, {.h24 = options.h24, .seconds = options.seconds});
  state.clockFamily = clockFontFamily(clock);
  state.clockWeight = clockWeightFor(clock, options.axes, local.tm_sec);
  state.clockTracking = clock.tracking;
  state.clockStroke = clock.stroke;
  if (clock.gradient || options.color == "gradient") {
    state.clockGradientFrom = gradientFrom;
    state.clockGradientTo = gradientTo;
  }
  if (options.color == "accent") {
    state.clockInk = kAccentTextInk;
  } else if (options.color == "wallpaper") {
    state.clockInk = gradientFrom;
  }
  state.clockVariations = clockVariationsFor(clock, options.axes);
  if (clock.breatheWidth && options.motion && options.axes.width <= 0) {
    state.clockVariations += std::format("{}wdth={}", state.clockVariations.empty() ? "" : ",",
                                         clockBreathWidth(clock, local.tm_sec));
  }
  state.clockTint = options.material == "tint";
  state.clockGlass = options.material == "glass";
  state.clockStretch = clock.stretch;
  state.clockMorph = clock.morph && options.motion;
  // The caret stays on without motion; with motion it blinks once a second.
  state.clockCaret = clock.caret && (!options.motion || local.tm_sec % 2 == 0);
  state.clockNeon = clock.neon;
  state.clockSplit = clock.splitOutline;
  state.clockGhost = clock.ghost;
  state.orbit = clock.layout == ClockLayout::Orbit;
  if (clock.layout == ClockLayout::WordGrid) {
    state.wordGridLit = clockWordGridLit(local);
  }
  state.clockTwoTone = clock.twoTone;
  state.flip = clock.layout == ClockLayout::Flip;
  if (clock.live && !options.seconds) {
    state.liveSeconds = std::format("{:02}", local.tm_sec);
  }
  if (clock.layout == ClockLayout::Analog || clock.layout == ClockLayout::Orbit) {
    state.analogHour = local.tm_hour;
    state.analogMinute = local.tm_min;
  }
  state.date = formatLocalTime("%A, %-d %B");
}
