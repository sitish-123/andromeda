#pragma once

#include "render/animation/animation.h"

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

  Id animate(
      float from, float to, float durationMs, Easing easing, std::function<void(float)> setter,
      std::function<void()> onComplete = {}, const void* owner = nullptr
  );
  // Real elapsed-time driver: ignores global motion enable/speed. Use for timeouts,
  // and for visual effects with a deliberate fixed duration that must stay independent
  // of the global animation settings (e.g. the wallpaper crossfade).
  Id animateTimer(
      float from, float to, float durationMs, Easing easing, std::function<void(float)> setter,
      std::function<void()> onComplete = {}, const void* owner = nullptr
  );
  // Spring motion: the spring sets the timing. Global motion speed scales it; reduced motion jumps to `to`.
  Id animateSpring(
      float from, float to, const desk::motion::SpringConfig& spring, std::function<void(float)> setter,
      std::function<void()> onComplete = {}, const void* owner = nullptr, float velocity = 0.0F
  );
  // Units per second of a running spring; 0 when `id` is not one.
  [[nodiscard]] float velocity(Id id) const;
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
