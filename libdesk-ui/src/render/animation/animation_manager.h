#pragma once

#include "render/animation/animation.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <vector>

class AnimationManager {
public:
  using Id = std::uint32_t;

  AnimationManager();
  ~AnimationManager();

  AnimationManager(const AnimationManager&) = delete;
  AnimationManager& operator=(const AnimationManager&) = delete;
  AnimationManager(AnimationManager&&) = delete;
  AnimationManager& operator=(AnimationManager&&) = delete;

  // Real elapsed-time driver: ignores global motion enable/speed. Use for timeouts, continuous loops and
  // fixed-duration effects that must stay independent of the global animation settings (e.g. the wallpaper crossfade).
  Id animateTimer(
      float from, float to, float durationMs, Easing easing, std::function<void(float)> setter,
      std::function<void()> onComplete = {}, const void* owner = nullptr
  );
  // Spring motion: the spring sets the timing. Global motion speed scales it; reduced motion jumps to `to`.
  Id animateSpring(
      float from, float to, const desk::motion::SpringConfig& spring, std::function<void(float)> setter,
      std::function<void()> onComplete = {}, const void* owner = nullptr, float velocity = 0.0F,
      Unit unit = Unit::Opacity
  );
  // Progress gate: a spring that runs `onPass` once when it has covered `fraction` of its travel, so a choreography
  // follows the shape and not the clock. `onPass` may start a child spring.
  Id animateGated(
      float from, float to, const desk::motion::SpringConfig& spring, std::function<void(float)> setter,
      float fraction, std::function<void()> onPass, std::function<void()> onComplete = {},
      const void* owner = nullptr, float velocity = 0.0F, Unit unit = Unit::Opacity
  );
  // Stagger for entries: child `index` starts 16 ms after child `index - 1`; children from the sixth on start together.
  // Exits never stagger. Without motion (off or reduced) every child starts at once.
  Id animateStaggered(
      int index, float from, float to, const desk::motion::SpringConfig& spring, std::function<void(float)> setter,
      std::function<void()> onComplete = {}, const void* owner = nullptr, Unit unit = Unit::Opacity
  );
  [[nodiscard]] static float staggerDelayMs(int index);
  // Units per second of a running spring; 0 when `id` is not one.
  [[nodiscard]] float velocity(Id id) const;
  // Spatial motion: a spring. With motion off or reduced it jumps to `to`; the caller crossfades on fade / fade_out.
  Id animateSpatial(
      float from, float to, const desk::motion::SpringConfig& spring, std::function<void(float)> setter,
      std::function<void()> onComplete = {}, const void* owner = nullptr, float velocity = 0.0F, Unit unit = Unit::Px
  );
  // Moves a running spring's target and keeps its velocity. False when `id` is not a running spring.
  bool retarget(Id id, float to);
  // Same, and switches to `spring` (for example expand -> collapse) without losing velocity.
  bool retarget(Id id, float to, const desk::motion::SpringConfig& spring);
  void cancel(Id id);
  void cancelAll();
  void reduceMotion();
  // Cancels any animations tagged with the given owner. Called from Node's destructor so that
  // animations holding a raw pointer to a scene node can never outlive their target.
  void cancelForOwner(const void* owner);
  void tick(float deltaMs);
  // Steps springs to `now`; a step longer than 50 ms counts as 50 ms.
  void tickAt(std::chrono::steady_clock::time_point now);
  [[nodiscard]] bool hasActive() const;

private:
  struct Entry {
    Id id = 0;
    const void* owner = nullptr;
    bool respectMotionEnabled = true;
    Animation animation;
  };

  std::vector<Entry> m_animations;
  Id m_nextId = 1;

  Id animateInternal(
      float from, float to, float durationMs, Easing easing, std::function<void(float)> setter,
      std::function<void()> onComplete, const void* owner, bool scaleDuration, bool respectMotionEnabled
  );
};
