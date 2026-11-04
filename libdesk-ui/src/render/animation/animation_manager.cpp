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
    std::function<void()> onComplete, const void* owner, float velocity, Threshold unit
) {
  if (!MotionService::instance().enabled()) {
    return jump(to, unit, std::move(setter), std::move(onComplete), owner);
  }
  desk::motion::Spring motion(from, to, spring, velocity);
  motion.setThreshold(unit.value);
  Id id = m_nextId++;
  m_animations.push_back(
      Entry{
          .id = id,
          .owner = owner,
          .respectMotionEnabled = true,
          .animation = Animation{
              .startValue = from,
              .endValue = to,
              .setter = std::move(setter),
              .onComplete = std::move(onComplete),
              .spring = motion,
              .lastStep = std::chrono::steady_clock::now(),
              .threshold = unit.value,
              .clamp01 = unit.clamp01,
          },
      }
  );
  return id;
}

AnimationManager::Id AnimationManager::jump(
    float to, Threshold unit, std::function<void(float)> setter, std::function<void()> onComplete, const void* owner,
    std::function<void()> onPass
) {
  if (setter) {
    setter(unit.clamp01 ? std::clamp(to, 0.0F, 1.0F) : to);
  }
  if (!onComplete && !onPass) {
    return 0;
  }
  const Id id = m_nextId++;
  m_animations.push_back(
      Entry{
          .id = id,
          .owner = owner,
          .respectMotionEnabled = true,
          .animation = Animation{
              .startValue = to,
              .endValue = to,
              .onComplete = std::move(onComplete),
              .spring = desk::motion::Spring(to, to, {}),
              .lastStep = std::chrono::steady_clock::now(),
              .gateFraction = onPass ? 0.0F : -1.0F,
              .onGate = std::move(onPass),
          },
      }
  );
  return id;
}

AnimationManager::Id AnimationManager::animateSpatial(
    float from, float to, const desk::motion::SpringConfig& spring, std::function<void(float)> setter,
    std::function<void()> onComplete, const void* owner, float velocity, Threshold unit
) {
  if (MotionService::instance().reduced()) {
    return jump(to, unit, std::move(setter), std::move(onComplete), owner);
  }
  return animateSpring(from, to, spring, std::move(setter), std::move(onComplete), owner, velocity, unit);
}

void AnimationManager::cancelRunning(Id id) {
  std::erase_if(m_animations, [id](const Entry& e) { return e.id == id && !e.animation.finished; });
}

AnimationManager::Id AnimationManager::drive(
    Id id, float from, float to, const desk::motion::SpringConfig& spring, std::function<void(float)> setter,
    std::function<void()> onComplete, const void* owner, float velocity, Threshold unit, bool spatial
) {
  const auto& motion = MotionService::instance();
  const bool instant = !motion.enabled() || (spatial && motion.reduced());
  if (!instant && id != 0) {
    for (auto& entry : m_animations) {
      if (entry.id == id && entry.animation.spring && !entry.animation.finished) {
        auto& anim = entry.animation;
        const float current = static_cast<float>(anim.spring->value());
        const float carried = velocity != 0.0F ? velocity : static_cast<float>(anim.spring->velocity());
        anim.spring = desk::motion::Spring(current, to, spring, carried);
        anim.spring->setThreshold(unit.value);
        anim.startValue = current;
        anim.endValue = to;
        anim.threshold = unit.value;
        anim.clamp01 = unit.clamp01;
        anim.gateFraction = -1.0F;
        anim.onGate = nullptr;
        anim.setter = std::move(setter);
        anim.onComplete = std::move(onComplete);
        return id;
      }
    }
  }
  if (id != 0) {
    cancelRunning(id);
  }
  if (instant) {
    return jump(to, unit, std::move(setter), std::move(onComplete), owner);
  }
  return animateSpring(from, to, spring, std::move(setter), std::move(onComplete), owner, velocity, unit);
}

AnimationManager::Id AnimationManager::animateGated(
    float from, float to, const desk::motion::SpringConfig& spring, std::function<void(float)> setter, float fraction,
    std::function<void()> onPass, std::function<void()> onComplete, const void* owner, float velocity, Threshold unit
) {
  if (!MotionService::instance().enabled()) {
    return jump(to, unit, std::move(setter), std::move(onComplete), owner, std::move(onPass));
  }
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
    std::function<void()> onComplete, const void* owner, Threshold unit
) {
  const auto& motion = MotionService::instance();
  const Id id = animateSpring(from, to, spring, std::move(setter), std::move(onComplete), owner, 0.0F, unit);
  if (id != 0 && motion.enabled() && !motion.reduced()) {
    m_animations.back().animation.delayMs = staggerDelayMs(index) / motion.speed();
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
  for (auto* list : m_dispatchLists) {
    for (auto& cb : *list) {
      cb.cancelled = cb.cancelled || cb.id == id;
    }
  }
  std::erase_if(m_animations, [id](const Entry& e) { return e.id == id; });
}

void AnimationManager::cancelAll() {
  for (auto* list : m_dispatchLists) {
    for (auto& cb : *list) {
      cb.cancelled = true;
    }
  }
  m_animations.clear();
}

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
  for (auto* list : m_dispatchLists) {
    for (auto& cb : *list) {
      cb.cancelled = cb.cancelled || cb.owner == owner;
    }
  }
  std::erase_if(m_animations, [owner](const Entry& e) { return e.owner == owner; });
}

void AnimationManager::tick(float /*deltaMs*/) { tickAt(std::chrono::steady_clock::now()); }

void AnimationManager::tickAt(std::chrono::steady_clock::time_point now) {
  // Collect completed callbacks separately — onComplete may call animate()
  // which would push_back and invalidate iterators during iteration.
  std::vector<Pending> completedCallbacks;
  std::vector<Pending> gateCallbacks;

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
        stepMs = 0.0F;
        anim.delayMs = 0.0F;
      }
      const float value = static_cast<float>(anim.spring->step(stepMs * MotionService::instance().speed() / 1000.0F));
      anim.finished = anim.spring->settled();
      if (anim.setter) {
        anim.setter(anim.clamp01 ? std::clamp(value, 0.0F, 1.0F) : value);
      }
      if (anim.onGate) {
        const float travel = anim.endValue - anim.startValue;
        const float progress = std::fabs(travel) > 1e-6F ? (value - anim.startValue) / travel : 1.0F;
        if (anim.finished || progress >= anim.gateFraction) {
          gateCallbacks.push_back({entry.id, entry.owner, std::move(anim.onGate)});
          anim.onGate = nullptr;
        }
      }
      if (anim.finished && anim.onComplete) {
        completedCallbacks.push_back({entry.id, entry.owner, std::move(anim.onComplete)});
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
      completedCallbacks.push_back({entry.id, entry.owner, std::move(anim.onComplete)});
    }
  }

  std::erase_if(m_animations, [](const Entry& e) { return e.animation.finished; });

  // A cancel from a callback marks the matching callbacks still pending in this list (and in outer ticks).
  std::vector<Pending> pending = std::move(gateCallbacks);
  pending.insert(
      pending.end(), std::make_move_iterator(completedCallbacks.begin()),
      std::make_move_iterator(completedCallbacks.end())
  );
  m_dispatchLists.push_back(&pending);
  for (std::size_t i = 0; i < pending.size(); ++i) {
    if (!pending[i].cancelled) {
      pending[i].fn();
    }
  }
  std::erase(m_dispatchLists, &pending);
}

bool AnimationManager::hasActive() const { return !m_animations.empty(); }
