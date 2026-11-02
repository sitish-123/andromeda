#include "render/animation/animation_manager.h"
#include "render/animation/motion_service.h"

#include <chrono>
#include <cmath>
#include <iostream>

namespace {

  bool check(bool cond, const char* msg) {
    if (!cond) {
      std::cerr << "FAIL: " << msg << '\n';
    }
    return cond;
  }

  bool nearlyEqual(float a, float b) { return std::fabs(a - b) < 0.0001F; }

  void resetMotion() {
    auto& motion = MotionService::instance();
    motion.setSpeed(1.0F);
    motion.setEnabled(true);
  }

} // namespace

int main() {
  bool ok = true;
  resetMotion();

  {
    AnimationManager manager;
    MotionService::instance().setEnabled(false);

    float value = 0.0F;
    bool completed = false;
    const desk::motion::SpringConfig spring{.damping = 0.8, .stiffness = 342.0};
    const auto id = manager.animateSpatial(
        0.0F, 10.0F, spring, [&value](float v) { value = v; }, [&completed]() { completed = true; }
    );

    ok &= check(id != 0, "reduced-motion animation with completion should remain cancellable");
    ok &= check(nearlyEqual(value, 10.0F), "reduced-motion animation did not snap to target");
    ok &= check(!completed, "reduced-motion completion ran synchronously");

    manager.tick(0.0F);

    ok &= check(completed, "reduced-motion completion did not run on tick");
    ok &= check(!manager.hasActive(), "reduced-motion animation stayed active after completion");
  }

  resetMotion();

  {
    AnimationManager manager;

    float value = 0.0F;
    bool completed = false;
    const desk::motion::SpringConfig spring{.damping = 0.8, .stiffness = 342.0};
    const auto id = manager.animateSpring(
        0.0F, 10.0F, spring, [&value](float v) { value = v; }, [&completed]() { completed = true; }
    );

    ok &= check(id != 0, "normal animation did not start");

    MotionService::instance().setEnabled(false);

    ok &= check(nearlyEqual(value, 10.0F), "active animation did not snap when motion was disabled");
    ok &= check(!completed, "active animation completion ran synchronously when motion was disabled");
    ok &= check(manager.hasActive(), "active animation did not remain pending for async completion");

    manager.tick(0.0F);

    ok &= check(completed, "active animation completion did not run after reduced-motion snap");
  }

  resetMotion();

  {
    AnimationManager manager;
    MotionService::instance().setEnabled(false);

    float value = -1.0F;
    bool completed = false;
    const auto id = manager.animateTimer(
        1.0F, 0.0F, 1000.0F, Easing::Linear, [&value](float v) { value = v; }, [&completed]() { completed = true; }
    );

    ok &= check(id != 0, "timer animation did not start while motion was disabled");
    ok &= check(nearlyEqual(value, -1.0F), "timer animation snapped when motion was disabled");

    manager.tick(0.0F);

    ok &= check(!completed, "timer animation completed early while motion was disabled");
    ok &= check(manager.hasActive(), "timer animation was removed early while motion was disabled");
    manager.cancel(id);
  }

  resetMotion();

  {
    // Progress gate: fires after the driver passes the fraction, not at a fixed time, and only once.
    AnimationManager manager;
    const desk::motion::SpringConfig spring{.damping = 0.8, .stiffness = 342.0};
    float value = 0.0F;
    int passed = 0;
    float valueAtPass = -1.0F;
    const auto t0 = std::chrono::steady_clock::now();
    manager.animateGated(
        0.0F, 100.0F, spring, [&value](float v) { value = v; }, 0.35F,
        [&] {
          ++passed;
          valueAtPass = value;
        },
        {}, nullptr, 0.0F, Unit::Px
    );
    for (int i = 1; i <= 200 && manager.hasActive(); ++i) {
      manager.tickAt(t0 + std::chrono::milliseconds(16 * i));
      ok &= check(passed == 0 || valueAtPass >= 35.0F, "gate fired before the fraction of travel");
    }
    ok &= check(passed == 1, "gate did not fire exactly once");
    ok &= check(nearlyEqual(value, 100.0F), "gated spring did not settle on its target");
  }

  {
    // Stagger: 16 ms apart, the sixth and later start together; the first child moves on the first step.
    ok &= check(nearlyEqual(AnimationManager::staggerDelayMs(0), 0.0F), "first child delayed");
    ok &= check(nearlyEqual(AnimationManager::staggerDelayMs(2), 32.0F), "stagger not 16 ms apart");
    ok &= check(
        nearlyEqual(AnimationManager::staggerDelayMs(5), AnimationManager::staggerDelayMs(40)),
        "children after the sixth did not start with the sixth"
    );
    AnimationManager manager;
    const desk::motion::SpringConfig spring{.damping = 0.8, .stiffness = 342.0};
    float first = 0.0F;
    float last = 0.0F;
    const auto t0 = std::chrono::steady_clock::now();
    manager.animateStaggered(0, 0.0F, 1.0F, spring, [&first](float v) { first = v; });
    manager.animateStaggered(5, 0.0F, 1.0F, spring, [&last](float v) { last = v; });
    manager.tickAt(t0 + std::chrono::milliseconds(16));
    ok &= check(first > 0.0F, "first staggered child did not start");
    ok &= check(nearlyEqual(last, 0.0F), "sixth staggered child started early");
    manager.tickAt(t0 + std::chrono::milliseconds(66));
    manager.tickAt(t0 + std::chrono::milliseconds(116));
    ok &= check(last > 0.0F, "sixth staggered child never started");
    manager.cancelAll();
  }

  {
    // Reduced motion: a spatial spring jumps (no bounce, no stagger); an effect spring still runs.
    AnimationManager manager;
    MotionService::instance().setReduced(true);
    const desk::motion::SpringConfig spring{.damping = 0.8, .stiffness = 342.0};
    float pos = 0.0F;
    float fade = 0.0F;
    manager.animateSpatial(0.0F, 50.0F, spring, [&pos](float v) { pos = v; });
    ok &= check(nearlyEqual(pos, 50.0F), "reduced motion: spatial spring did not jump");
    manager.animateSpring(0.0F, 1.0F, spring, [&fade](float v) { fade = v; });
    manager.tickAt(std::chrono::steady_clock::now() + std::chrono::milliseconds(16));
    ok &= check(fade < 1.0F, "reduced motion: effect spring should still crossfade");
    float stagger = -1.0F;
    manager.animateStaggered(5, 0.0F, 1.0F, spring, [&stagger](float v) { stagger = v; });
    manager.tickAt(std::chrono::steady_clock::now() + std::chrono::milliseconds(16));
    ok &= check(stagger > 0.0F, "reduced motion: stagger delayed");
    manager.cancelAll();
    MotionService::instance().setReduced(false);
  }

  {
    // Idle threshold: a spring stops at 0.5 px of its target and lands exactly on it.
    AnimationManager manager;
    const desk::motion::SpringConfig spring{.damping = 0.8, .stiffness = 584.0};
    float value = 0.0F;
    bool done = false;
    const auto t0 = std::chrono::steady_clock::now();
    manager.animateSpring(0.0F, 100.0F, spring, [&value](float v) { value = v; }, [&done] { done = true; }, nullptr, 0.0F, Unit::Px);
    for (int i = 1; i <= 100 && !done; ++i) {
      manager.tickAt(t0 + std::chrono::milliseconds(16 * i));
    }
    ok &= check(manager.hasActive() == false && value == 100.0F, "spring did not land exactly on its target");
  }

  {
    // A stall counts as 50 ms: one huge step does not jump to the target.
    AnimationManager manager;
    const desk::motion::SpringConfig spring{.damping = 1.0, .stiffness = 100.0};
    float value = 0.0F;
    const auto t0 = std::chrono::steady_clock::now();
    manager.animateSpring(0.0F, 100.0F, spring, [&value](float v) { value = v; });
    manager.tickAt(t0 + std::chrono::seconds(10));
    ok &= check(value < 50.0F, "dt was not clamped after a stall");
    manager.cancelAll();
  }

  resetMotion();
  return ok ? 0 : 1;
}
