#include "render/animation/animation_manager.h"

#include "ui/style.h"
#include "render/animation/motion_service.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace {
  constexpr float kReducedMotionDurationMs = 1.0F;
}

AnimationManager::AnimationManager() { MotionService::instance().registerManager(this); }

AnimationManager::~AnimationManager() { MotionService::instance().unregisterManager(this); }

AnimationManager::Id AnimationManager::animateTimer(
    float from, float to, float durationMs, Easing easing, std::function<void(float)> setter,
    std::function<void()> onComplete, const void* owner
) {
  return animateInternal(from, to, durationMs, easing, std::move(setter), std::move(onComplete), owner, false, false);
}

AnimationManager::Id AnimationManager::animateInternal(
    float from, float to, float durationMs, Easing easing, std::function<void(float)> setter,
    std::function<void()> onComplete, const void* owner, bool scaleDuration, bool respectMotionEnabled
) {
  const auto& motion = MotionService::instance();
  const bool reduceMotion = respectMotionEnabled && !motion.enabled();
  if (reduceMotion) {
    if (setter) {
      setter(to);
    }
    if (!onComplete) {
      return 0;
    }
  }

  float effectiveDurationMs = scaleDuration ? durationMs / motion.speed() : durationMs;
  if (reduceMotion) {
    effectiveDurationMs = kReducedMotionDurationMs;
  }

  if (effectiveDurationMs <= 0.0F) {
    if (setter) {
      setter(to);
    }
    if (onComplete) {
      onComplete();
    }
    return 0;
  }

  Id id = m_nextId++;
  const auto now = std::chrono::steady_clock::now();
  m_animations.push_back(
      Entry{
          .id = id,
          .owner = owner,
          .respectMotionEnabled = respectMotionEnabled,
          .animation = Animation{
              .startValue = reduceMotion ? to : from,
              .endValue = to,
              .durationMs = effectiveDurationMs,
              .startedAt = reduceMotion ? now
                      - std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                              std::chrono::duration<float, std::milli>(kReducedMotionDurationMs)
                      )
                                        : now,
              .easing = easing,
              .setter = std::move(setter),
              .onComplete = std::move(onComplete),
          },
      }
  );
  return id;
}

AnimationManager::Id AnimationManager::animateSpring(
    float from, float to, const desk::motion::SpringConfig& spring, std::function<void(float)> setter,
    std::function<void()> onComplete, const void* owner, float velocity, Unit unit
) {
  const float start = MotionService::instance().enabled() ? from : to;
  const float threshold = unitThreshold(unit);
  desk::motion::Spring motion(start, to, spring, velocity);
  motion.setThreshold(threshold);
  Id id = m_nextId++;
  m_animations.push_back(
      Entry{
          .id = id,
          .owner = owner,
          .respectMotionEnabled = true,
          .animation = Animation{
              .startValue = start,
              .endValue = to,
              .setter = std::move(setter),
              .onComplete = std::move(onComplete),
              .spring = motion,
              .lastStep = std::chrono::steady_clock::now(),
              .threshold = threshold,
          },
      }
  );
  return id;
}

AnimationManager::Id AnimationManager::animateSpatial(
    float from, float to, const desk::motion::SpringConfig& spring, std::function<void(float)> setter,
    std::function<void()> onComplete, const void* owner, float velocity, Unit unit
) {
  const auto& motion = MotionService::instance();
  if (motion.enabled() && !motion.reduced()) {
    return animateSpring(from, to, spring, std::move(setter), std::move(onComplete), owner, velocity, unit);
  }
  if (setter) {
    setter(to);
  }
  return animateSpring(to, to, spring, {}, std::move(onComplete), owner, 0.0F, unit);
}

AnimationManager::Id AnimationManager::animateGated(
    float from, float to, const desk::motion::SpringConfig& spring, std::function<void(float)> setter, float fraction,
    std::function<void()> onPass, std::function<void()> onComplete, const void* owner, float velocity, Unit unit
) {
  const Id id = animateSpring(from, to, spring, std::move(setter), std::move(onComplete), owner, velocity, unit);
  auto& anim = m_animations.back().animation;
  anim.gateFraction = std::clamp(fraction, 0.0F, 1.0F);
  anim.onGate = std::move(onPass);
  return id;
}

float AnimationManager::staggerDelayMs(int index) {
  constexpr float kStaggerMs = 16.0F;
  constexpr int kMaxStaggered = 6;
  return kStaggerMs * static_cast<float>(std::clamp(index, 0, kMaxStaggered - 1));
}

AnimationManager::Id AnimationManager::animateStaggered(
    int index, float from, float to, const desk::motion::SpringConfig& spring, std::function<void(float)> setter,
    std::function<void()> onComplete, const void* owner, Unit unit
) {
  const auto& motion = MotionService::instance();
  const Id id = animateSpring(from, to, spring, std::move(setter), std::move(onComplete), owner, 0.0F, unit);
  if (motion.enabled() && !motion.reduced()) {
    m_animations.back().animation.delayMs = staggerDelayMs(index);
  }
  return id;
}

float AnimationManager::velocity(Id id) const {
  for (const auto& entry : m_animations) {
    if (entry.id == id && entry.animation.spring && !entry.animation.finished) {
      return static_cast<float>(entry.animation.spring->velocity());
    }
  }
  return 0.0F;
}

bool AnimationManager::retarget(Id id, float to) {
  for (auto& entry : m_animations) {
    if (entry.id == id && entry.animation.spring && !entry.animation.finished) {
      entry.animation.endValue = to;
      if (MotionService::instance().enabled()) {
        entry.animation.spring->retarget(to);
      } else {
        entry.animation.spring = desk::motion::Spring(to, to, {});
      }
      return true;
    }
  }
  return false;
}

bool AnimationManager::retarget(Id id, float to, const desk::motion::SpringConfig& spring) {
  for (auto& entry : m_animations) {
    if (entry.id == id && entry.animation.spring && !entry.animation.finished) {
      entry.animation.endValue = to;
      if (MotionService::instance().enabled()) {
        entry.animation.spring = desk::motion::Spring(
            entry.animation.spring->value(), to, spring, entry.animation.spring->velocity()
        );
        entry.animation.spring->setThreshold(entry.animation.threshold);
      } else {
        entry.animation.spring = desk::motion::Spring(to, to, {});
      }
      return true;
    }
  }
  return false;
}

void AnimationManager::cancel(Id id) {
  std::erase_if(m_animations, [id](const Entry& e) { return e.id == id; });
}

void AnimationManager::cancelAll() { m_animations.clear(); }

void AnimationManager::reduceMotion() {
  const auto now = std::chrono::steady_clock::now();
  for (auto& entry : m_animations) {
    if (!entry.respectMotionEnabled || entry.animation.finished) {
      continue;
    }

    auto& anim = entry.animation;
    if (anim.setter) {
      anim.setter(anim.endValue);
    }
    if (anim.spring) {
      anim.spring = desk::motion::Spring(anim.endValue, anim.endValue, {});
      anim.delayMs = 0.0F;
      continue;
    }
    anim.startValue = anim.endValue;
    anim.durationMs = kReducedMotionDurationMs;
    anim.elapsedMs = 0.0F;
    anim.startedAt = now
        - std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                         std::chrono::duration<float, std::milli>(kReducedMotionDurationMs)
        );
  }
}

void AnimationManager::cancelForOwner(const void* owner) {
  if (owner == nullptr) {
    return;
  }
  std::erase_if(m_animations, [owner](const Entry& e) { return e.owner == owner; });
}

void AnimationManager::tick(float /*deltaMs*/) { tickAt(std::chrono::steady_clock::now()); }

void AnimationManager::tickAt(std::chrono::steady_clock::time_point now) {
  // Collect completed callbacks separately — onComplete may call animate()
  // which would push_back and invalidate iterators during iteration.
  std::vector<std::function<void()>> completedCallbacks;
  std::vector<std::function<void()>> gateCallbacks;

  for (auto& entry : m_animations) {
    auto& anim = entry.animation;
    if (anim.finished) {
      continue;
    }

    if (anim.spring) {
      constexpr float kMaxStepMs = 50.0F;
      float stepMs = std::clamp(std::chrono::duration<float, std::milli>(now - anim.lastStep).count(), 0.0F, kMaxStepMs);
      anim.lastStep = now;
      if (anim.delayMs > 0.0F) {
        anim.delayMs -= stepMs;
        if (anim.delayMs > 0.0F) {
          continue;
        }
        stepMs = -anim.delayMs;
        anim.delayMs = 0.0F;
      }
      const float value = static_cast<float>(anim.spring->step(stepMs * MotionService::instance().speed() / 1000.0F));
      anim.finished = anim.spring->settled();
      if (anim.setter) {
        anim.setter(value);
      }
      if (anim.onGate) {
        const float travel = anim.endValue - anim.startValue;
        const float progress = std::fabs(travel) > 1e-6F ? (value - anim.startValue) / travel : 1.0F;
        if (anim.finished || progress >= anim.gateFraction) {
          gateCallbacks.push_back(std::move(anim.onGate));
          anim.onGate = nullptr;
        }
      }
      if (anim.finished && anim.onComplete) {
        completedCallbacks.push_back(std::move(anim.onComplete));
      }
      continue;
    }

    // Progress from wall time so a fixed duration stays correct when the compositor
    // delivers sparse `wl_surface.frame` callbacks (common right after cold boot).
    const float wallElapsedMs = std::chrono::duration<float, std::milli>(now - anim.startedAt).count();
    anim.elapsedMs = wallElapsedMs;
    float t = anim.durationMs > 0.0F ? wallElapsedMs / anim.durationMs : 1.0F;

    if (t >= 1.0F) {
      t = 1.0F;
      anim.finished = true;
    }

    float easedT = applyEasing(anim.easing, t);
    float value = anim.startValue + (anim.endValue - anim.startValue) * easedT;

    if (anim.setter) {
      anim.setter(value);
    }

    if (anim.finished && anim.onComplete) {
      completedCallbacks.push_back(std::move(anim.onComplete));
    }
  }

  std::erase_if(m_animations, [](const Entry& e) { return e.animation.finished; });

  for (auto& cb : gateCallbacks) {
    cb();
  }
  for (auto& cb : completedCallbacks) {
    cb();
  }
}

bool AnimationManager::hasActive() const { return !m_animations.empty(); }
