#pragma once

#include <optional>

namespace andromeda {

  // Holding and Gated arm the release timer (3 s, then 300 ms after the wallpaper maps); AwaitingGreeter still holds
  // after the timer fired while the greeter showed nothing; Crossing steps the cross on frames; Gone holds no frame.
  enum class HeldFramePhase { Holding, Gated, AwaitingGreeter, Crossing, Gone };
  enum class HeldFrameEvent { WallpaperMapped, TimerFired, TimerFiredGreeterEmpty, GreeterShown, CrossSettled, Dropped };

  [[nodiscard]] constexpr bool heldFrameTimerArmed(HeldFramePhase phase) {
    return phase == HeldFramePhase::Holding || phase == HeldFramePhase::Gated;
  }

  // Rearm sets the 300 ms gate; Remove deletes the timer source.
  enum class HeldFrameTimer { Keep, Rearm, Remove };
  struct HeldFrameStep {
    HeldFramePhase phase;
    HeldFrameTimer timer;
  };

  // The next phase and what to do with the timer, or nothing for an event the phase ignores.
  [[nodiscard]] constexpr std::optional<HeldFrameStep> heldFrameNext(HeldFramePhase phase, HeldFrameEvent event) {
    std::optional<HeldFramePhase> next;
    switch (event) {
    case HeldFrameEvent::WallpaperMapped:
      next = phase == HeldFramePhase::Holding ? std::optional{HeldFramePhase::Gated} : std::nullopt;
      break;
    case HeldFrameEvent::TimerFired:
      next = heldFrameTimerArmed(phase) ? std::optional{HeldFramePhase::Crossing} : std::nullopt;
      break;
    case HeldFrameEvent::TimerFiredGreeterEmpty:
      next = heldFrameTimerArmed(phase) ? std::optional{HeldFramePhase::AwaitingGreeter} : std::nullopt;
      break;
    case HeldFrameEvent::GreeterShown:
      next = phase == HeldFramePhase::AwaitingGreeter ? std::optional{HeldFramePhase::Crossing} : std::nullopt;
      break;
    case HeldFrameEvent::CrossSettled:
      next = phase == HeldFramePhase::Crossing ? std::optional{HeldFramePhase::Gone} : std::nullopt;
      break;
    case HeldFrameEvent::Dropped:
      next = phase != HeldFramePhase::Gone ? std::optional{HeldFramePhase::Gone} : std::nullopt;
      break;
    }
    if (!next.has_value()) {
      return std::nullopt;
    }
    HeldFrameTimer timer = HeldFrameTimer::Keep;
    if (*next == HeldFramePhase::Gated) {
      timer = HeldFrameTimer::Rearm;
    } else if (heldFrameTimerArmed(phase) && !heldFrameTimerArmed(*next)) {
      timer = HeldFrameTimer::Remove;
    }
    return HeldFrameStep{.phase = *next, .timer = timer};
  }

} // namespace andromeda
