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
      Threshold unit = Unit::Opacity
  );
  // Progress gate: a spring that runs `onPass` once when it has covered `fraction` of its travel, so a choreography
  // follows the shape and not the clock. `onPass` may start a child spring.
  Id animateGated(
      float from, float to, const desk::motion::SpringConfig& spring, std::function<void(float)> setter,
      float fraction, std::function<void()> onPass, std::function<void()> onComplete = {},
      const void* owner = nullptr, float velocity = 0.0F, Threshold unit = Unit::Opacity
  );
  // Stagger for entries: child `index` starts 16 ms after child `index - 1`; children from the sixth on start together.
  // Exits never stagger. Without motion (off or reduced) every child starts at once.
  Id animateStaggered(
      int index, float from, float to, const desk::motion::SpringConfig& spring, std::function<void(float)> setter,
      std::function<void()> onComplete = {}, const void* owner = nullptr, Threshold unit = Unit::Opacity
  );
  [[nodiscard]] static float staggerDelayMs(int index);
  // Retarget, never restart: when `id` is a running spring, it moves to `to` and takes the new `setter`, `onComplete`,
  // `spring`, `unit` and gate base. It keeps its velocity unless `velocity` is non-zero. The replaced `onComplete`
  // is dropped and never called, so one `onComplete` must do the same job for every direction. Otherwise a new
  // spring starts at `from`. Returns the id (0 when motion is off and no completion is pending).
  Id drive(
      Id id, float from, float to, const desk::motion::SpringConfig& spring, std::function<void(float)> setter,
      std::function<void()> onComplete = {}, const void* owner = nullptr, float velocity = 0.0F,
      Threshold unit = Unit::Opacity, bool spatial = false
  );
  // Units per second of a running spring; 0 when `id` is not one.
  [[nodiscard]] float velocity(Id id) const;
  // Spatial motion: a spring. With motion off or reduced it jumps to `to`; the caller crossfades on fade / fade_out.
  Id animateSpatial(
      float from, float to, const desk::motion::SpringConfig& spring, std::function<void(float)> setter,
      std::function<void()> onComplete = {}, const void* owner = nullptr, float velocity = 0.0F, Threshold unit = Unit::Scale
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

  struct Pending {
    Id id = 0;
    const void* owner = nullptr;
    std::function<void()> fn;
    bool cancelled = false;
  };

  std::vector<Entry> m_animations;
  // Callback lists being dispatched (nested tickAt calls stack); a cancel marks the matching pending entries.
  std::vector<std::vector<Pending>*> m_dispatchLists;
  Id m_nextId = 1;

  // Motion off: runs the setter now and queues `onComplete` (and `onPass`) for the next tick as a settled entry.
  // The returned id is non-zero while that completion is pending, so a caller's `m_id = ...` never overwrites an id
  // the callback stored. 0 when there is no callback to run.
  Id jump(
      float to, Threshold unit, std::function<void(float)> setter, std::function<void()> onComplete,
      const void* owner, std::function<void()> onPass = {}
  );
  void cancelRunning(Id id);
  Id animateInternal(
      float from, float to, float durationMs, Easing easing, std::function<void(float)> setter,
      std::function<void()> onComplete, const void* owner, bool scaleDuration, bool respectMotionEnabled
  );
};
