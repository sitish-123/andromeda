#pragma once

#include "motion/desk_motion.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>

enum class Easing : std::uint8_t {
  Linear,
  EaseInQuad,
  EaseOutQuad,
  EaseInOutQuad,
  EaseInCubic,
  EaseOutCubic,
  EaseInOutCubic,
  EaseOutBack,
};

float applyEasing(Easing easing, float t);

// What a spring animates; sets its idle threshold (rulebook 7.1 rule 9).
enum class Unit : std::uint8_t {
  Opacity, // 1/256
  Scale,   // 0.002
  Px,      // 0.5
  Progress, // 1/256, and the value stays in [0, 1] (reveals, opacity driven by a bouncing spring)
};

struct Threshold {
  float value = 1.0F / 256.0F;
  bool clamp01 = false;

  constexpr Threshold() = default;
  constexpr Threshold(Unit unit) { // NOLINT(google-explicit-constructor): a Unit is a Threshold
    switch (unit) {
    case Unit::Px: value = 0.5F; break;
    case Unit::Scale: value = 0.002F; break;
    case Unit::Progress: clamp01 = true; break;
    case Unit::Opacity: break;
    }
  }
  // A 0..1 progress that maps to `travelPx` pixels: 0.5 px of travel.
  [[nodiscard]] static constexpr Threshold px(float travelPx) {
    Threshold t;
    t.value = 0.5F / (travelPx > 0.5F ? travelPx : 0.5F);
    t.clamp01 = true;
    return t;
  }
};

struct Animation {
  float startValue = 0.0F;
  float endValue = 0.0F;
  float durationMs = 0.0F;
  float elapsedMs = 0.0F;
  std::chrono::steady_clock::time_point startedAt;
  Easing easing = Easing::EaseOutQuad;
  std::function<void(float)> setter;
  std::function<void()> onComplete;
  bool finished = false;
  // Set for spring animations: the spring owns the timing and durationMs/easing do not apply.
  std::optional<desk::motion::Spring> spring;
  std::chrono::steady_clock::time_point lastStep;
  float threshold = 0.0F;
  bool clamp01 = false;
  // Stagger: the spring holds until this much time has passed.
  float delayMs = 0.0F;
  // Progress gate: runs once when the travel passes this fraction (0..1); negative = no gate.
  float gateFraction = -1.0F;
  std::function<void()> onGate;
};
