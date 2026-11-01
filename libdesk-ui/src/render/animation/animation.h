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
};
