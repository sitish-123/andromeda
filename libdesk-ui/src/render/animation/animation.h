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
};

[[nodiscard]] constexpr float unitThreshold(Unit unit) {
  switch (unit) {
  case Unit::Px: return 0.5F;
  case Unit::Scale: return 0.002F;
  case Unit::Opacity: break;
  }
  return 1.0F / 256.0F;
}

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
  // Stagger: the spring holds until this much time has passed.
  float delayMs = 0.0F;
  // Progress gate: runs once when the travel passes this fraction (0..1); negative = no gate.
  float gateFraction = -1.0F;
  std::function<void()> onGate;
};
