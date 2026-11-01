#pragma once
// libdesk-motion: the one curve and spring solver shared by the compositor, the shell toolkit and our apps.
// Pure C++, no dependencies.

#include <array>
#include <cstddef>
#include <cstdint>

namespace desk::motion {

  enum class Easing : uint8_t {
    Linear,
    EaseInSine,
    EaseOutSine,
    EaseInOutSine,
    EaseInQuad,
    EaseOutQuad,
    EaseInOutQuad,
    EaseInCubic,
    EaseOutCubic,
    EaseInOutCubic,
    EaseInQuart,
    EaseOutQuart,
    EaseInOutQuart,
    EaseInQuint,
    EaseOutQuint,
    EaseInOutQuint,
    EaseInExpo,
    EaseOutExpo,
    EaseInOutExpo,
    EaseInCirc,
    EaseOutCirc,
    EaseInOutCirc,
    EaseInBack,
    EaseOutBack,
    EaseInOutBack,
    EaseInElastic,
    EaseOutElastic,
    EaseInOutElastic,
    EaseInBounce,
    EaseOutBounce,
    EaseInOutBounce,
    Snappy,
    CustomBezier,
    Spring
  };

  struct BezierCurve {
    double x1 = 0.05;
    double y1 = 0.9;
    double x2 = 0.1;
    double y2 = 1.05;
    bool operator==(const BezierCurve&) const = default;
  };

  struct SpringConfig {
    double damping = 0.75;
    double stiffness = 100.0;
    double mass = 1.0;
    bool operator==(const SpringConfig&) const = default;
  };

  struct AnimationCurve {
    Easing easing = Easing::EaseOutCubic;
    BezierCurve bezier{};
    SpringConfig spring{};
    bool operator==(const AnimationCurve&) const = default;
  };

  // Standalone math and evaluation functions
  [[nodiscard]] double solveCubicBezier(double x1, double y1, double x2, double y2, double x);
  // Wall-clock length of a spring curve: the time a unit step from rest needs to settle within
  // kSpringSettleEpsilon of its target. Stiffness and mass set that timescale, damping only the shape, so a spring
  // curve owns its duration and the configured duration_ms does not apply to it. Clamped to the duration_ms range.
  [[nodiscard]] int springDurationMs(const SpringConfig& config);
  [[nodiscard]] double solveSpringPhysics(
      double from, double to, double velocity, double elapsedSec, const SpringConfig& config,
      double* outVelocity = nullptr
  );
  // Upper bound on the displacement a damped spring can reach from its current position and velocity. The bound is
  // expressed in the same units as current and target and follows from the spring's remaining mechanical energy.
  [[nodiscard]] double
  springDisplacementBound(double current, double target, double velocity, const SpringConfig& config);
  [[nodiscard]] double applyEasing(const AnimationCurve& curve, double progress);
  [[nodiscard]] inline double evaluateCurve(const AnimationCurve& curve, double progress) {
    return applyEasing(curve, progress);
  }

  // Logical layout geometry must remain between its endpoints and advance in one direction. Curves that reverse or
  // overshoot are projected onto cumulative travel, so their timing remains visible for the full configured duration
  // without letting tiled boxes cross a protected boundary. Already bounded monotonic curves are evaluated directly.
  class MonotonicEasing {
  public:
    MonotonicEasing();
    explicit MonotonicEasing(const AnimationCurve& curve);

    void reset(const AnimationCurve& curve);
    [[nodiscard]] double value(double linearProgress) const;

  private:
    static constexpr std::size_t kSampleCount = 512;

    AnimationCurve m_curve{.easing = Easing::Linear};
    std::array<double, kSampleCount + 1> m_progress{};
    bool m_direct = true;
  };

  // A spring that keeps its velocity when its target changes mid-flight.
  class Spring {
  public:
    Spring() = default;
    // velocity: units per second at the start, for example a finger's speed at release.
    Spring(double from, double to, const SpringConfig& config, double velocity = 0.0);

    // Moves the target; the motion continues from the current position and velocity.
    void retarget(double to);
    // Advances by `seconds` and returns the new position.
    double step(double seconds);
    [[nodiscard]] double value() const { return m_value; }
    [[nodiscard]] double target() const { return m_to; }
    [[nodiscard]] double velocity() const { return m_velocity; }
    [[nodiscard]] bool settled() const { return m_settled; }

  private:
    SpringConfig m_config{};
    double m_from = 0.0;
    double m_to = 0.0;
    double m_value = 0.0;
    double m_velocity = 0.0;
    double m_startVelocity = 0.0;
    double m_elapsed = 0.0;
    bool m_settled = true;
  };

} // namespace desk::motion
