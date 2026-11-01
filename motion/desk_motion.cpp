#include "motion/desk_motion.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace desk::motion {

  namespace {
    constexpr double kPi = std::numbers::pi;
    // Energy the unit step may keep and still count as settled, in target units. Matches the residual
    // solveSpringPhysics itself treats as arrived.
    constexpr double kSpringSettleEpsilon = 1e-4;
    // A spring's derived duration stays inside the duration_ms range a configured event accepts.
    constexpr int kSpringMinDurationMs = 1;
    constexpr int kSpringMaxDurationMs = 10000;
  } // namespace

  double solveCubicBezier(double x1, double y1, double x2, double y2, double x) {
    if (x <= 0.0) {
      return 0.0;
    }
    if (x >= 1.0) {
      return 1.0;
    }
    if (!std::isfinite(x)) {
      return 0.0;
    }

    const double cx1 = std::clamp(x1, 0.0, 1.0);
    const double cx2 = std::clamp(x2, 0.0, 1.0);

    const double cx = 3.0 * cx1;
    const double bx = 3.0 * (cx2 - cx1) - cx;
    const double ax = 1.0 - cx - bx;

    const double cy = 3.0 * y1;
    const double by = 3.0 * (y2 - y1) - cy;
    const double ay = 1.0 - cy - by;

    auto evalX = [ax, bx, cx](double t) noexcept { return ((ax * t + bx) * t + cx) * t; };
    auto evalY = [ay, by, cy](double t) noexcept { return ((ay * t + by) * t + cy) * t; };
    auto evalDx = [ax, bx, cx](double t) noexcept { return (3.0 * ax * t + 2.0 * bx) * t + cx; };

    // Newton-Raphson iteration (fast quadratic convergence)
    double t = x;
    for (int i = 0; i < 8; ++i) {
      const double currentX = evalX(t) - x;
      if (std::abs(currentX) < 1e-7) {
        return evalY(t);
      }
      const double dX = evalDx(t);
      if (std::abs(dX) < 1e-6) {
        break;
      }
      const double nextT = t - currentX / dX;
      if (nextT < 0.0 || nextT > 1.0) {
        break;
      }
      t = nextT;
    }

    // Bounded bisection fallback (guaranteed monotonic convergence)
    double minT = 0.0;
    double maxT = 1.0;
    t = x;
    for (int i = 0; i < 12; ++i) {
      const double guessX = evalX(t);
      if (std::abs(guessX - x) < 1e-7) {
        return evalY(t);
      }
      if (x > guessX) {
        minT = t;
      } else {
        maxT = t;
      }
      t = 0.5 * (minT + maxT);
    }
    return evalY(t);
  }

  int springDurationMs(const SpringConfig& config) {
    thread_local SpringConfig cachedConfig{};
    thread_local int cachedMs = 0;
    if (cachedMs != 0 && config == cachedConfig) {
      return cachedMs;
    }

    const double mass = std::max(1e-4, std::isfinite(config.mass) ? config.mass : 1.0);
    const double stiffness = std::max(1e-4, std::isfinite(config.stiffness) ? config.stiffness : 100.0);
    // Remaining mechanical energy of the unit step, which decays monotonically, so the settle time is bisectable.
    const auto remaining = [&config, mass, stiffness](double seconds) {
      double velocity = 0.0;
      const double position = solveSpringPhysics(0.0, 1.0, 0.0, seconds, config, &velocity);
      return std::hypot(1.0 - position, velocity * std::sqrt(mass / stiffness));
    };

    constexpr double kMaxSeconds = static_cast<double>(kSpringMaxDurationMs) / 1000.0;
    double settled = std::min(kMaxSeconds, std::sqrt(mass / stiffness));
    while (settled < kMaxSeconds && remaining(settled) > kSpringSettleEpsilon) {
      settled = std::min(kMaxSeconds, settled * 2.0);
    }
    int durationMs = kSpringMaxDurationMs;
    if (remaining(settled) <= kSpringSettleEpsilon) {
      double unsettled = 0.0;
      for (int i = 0; i < 40; ++i) {
        const double middle = 0.5 * (unsettled + settled);
        if (remaining(middle) > kSpringSettleEpsilon) {
          unsettled = middle;
        } else {
          settled = middle;
        }
      }
      durationMs = static_cast<int>(std::ceil(settled * 1000.0));
    }

    cachedConfig = config;
    cachedMs = std::clamp(durationMs, kSpringMinDurationMs, kSpringMaxDurationMs);
    return cachedMs;
  }

  double solveSpringPhysics(
      double from, double to, double velocity, double elapsedSec, const SpringConfig& config, double* outVelocity
  ) {
    if (!std::isfinite(from) || !std::isfinite(to) || !std::isfinite(velocity)) {
      if (outVelocity != nullptr) {
        *outVelocity = 0.0;
      }
      return to;
    }
    if (elapsedSec <= 0.0) {
      if (outVelocity != nullptr) {
        *outVelocity = velocity;
      }
      return from;
    }

    const double m = std::max(1e-4, std::isfinite(config.mass) ? config.mass : 1.0);
    const double k = std::max(1e-4, std::isfinite(config.stiffness) ? config.stiffness : 100.0);
    const double w0 = std::sqrt(k / m);
    const double zeta = std::max(0.0, std::isfinite(config.damping) ? config.damping : 0.75);
    const double beta = zeta * w0;
    const double x0 = from - to;
    const double v0 = velocity;
    const double t = elapsedSec;
    const double env = std::exp(-beta * t);

    double posOffset = 0.0;
    double vel = 0.0;

    if (std::abs(zeta - 1.0) < 1e-5) {
      // Critically damped
      posOffset = env * (x0 + (v0 + w0 * x0) * t);
      vel = env * (v0 - (v0 + w0 * x0) * w0 * t);
    } else if (zeta < 1.0) {
      // Underdamped
      const double wd = w0 * std::sqrt(1.0 - zeta * zeta);
      const double sinVal = std::sin(wd * t);
      const double cosVal = std::cos(wd * t);
      const double c2 = (v0 + beta * x0) / wd;
      posOffset = env * (x0 * cosVal + c2 * sinVal);
      vel = env * ((v0 * cosVal) - (x0 * wd + beta * c2) * sinVal);
    } else {
      // Overdamped
      const double wd = w0 * std::sqrt(zeta * zeta - 1.0);
      const double sinhVal = std::sinh(wd * t);
      const double coshVal = std::cosh(wd * t);
      const double c2 = (v0 + beta * x0) / wd;
      posOffset = env * (x0 * coshVal + c2 * sinhVal);
      vel = env * (v0 * coshVal + (x0 * wd - beta * c2) * sinhVal);
    }

    if (std::abs(posOffset) < 1e-4 && std::abs(vel) < 1e-4) {
      if (outVelocity != nullptr) {
        *outVelocity = 0.0;
      }
      return to;
    }

    if (outVelocity != nullptr) {
      *outVelocity = vel;
    }
    return to + posOffset;
  }

  double springDisplacementBound(double current, double target, double velocity, const SpringConfig& config) {
    if (!std::isfinite(current) || !std::isfinite(target) || !std::isfinite(velocity)) {
      return std::numeric_limits<double>::infinity();
    }
    const double mass = std::max(1e-4, std::isfinite(config.mass) ? config.mass : 1.0);
    const double stiffness = std::max(1e-4, std::isfinite(config.stiffness) ? config.stiffness : 100.0);
    return std::hypot(current - target, velocity * std::sqrt(mass / stiffness));
  }

  double applyEasing(const AnimationCurve& curve, double progress) {
    const double linear = std::clamp(progress, 0.0, 1.0);

    switch (curve.easing) {
    case Easing::Linear:
      return linear;

    // Sine
    case Easing::EaseInSine:
      return 1.0 - std::cos((linear * kPi) / 2.0);
    case Easing::EaseOutSine:
      return std::sin((linear * kPi) / 2.0);
    case Easing::EaseInOutSine:
      return -(std::cos(kPi * linear) - 1.0) / 2.0;

    // Quad
    case Easing::EaseInQuad:
      return linear * linear;
    case Easing::EaseOutQuad: {
      const double remaining = 1.0 - linear;
      return 1.0 - remaining * remaining;
    }
    case Easing::EaseInOutQuad:
      return linear < 0.5 ? 2.0 * linear * linear : 1.0 - std::pow(-2.0 * linear + 2.0, 2) / 2.0;

    // Cubic
    case Easing::EaseInCubic:
      return linear * linear * linear;
    case Easing::EaseOutCubic: {
      const double remaining = 1.0 - linear;
      return 1.0 - remaining * remaining * remaining;
    }
    case Easing::EaseInOutCubic:
      return linear < 0.5 ? 4.0 * linear * linear * linear : 1.0 - std::pow(-2.0 * linear + 2.0, 3) / 2.0;

    // Quart
    case Easing::EaseInQuart:
      return linear * linear * linear * linear;
    case Easing::EaseOutQuart: {
      const double remaining = 1.0 - linear;
      return 1.0 - remaining * remaining * remaining * remaining;
    }
    case Easing::EaseInOutQuart:
      return linear < 0.5 ? 8.0 * linear * linear * linear * linear : 1.0 - std::pow(-2.0 * linear + 2.0, 4) / 2.0;

    // Quint
    case Easing::EaseInQuint:
      return std::pow(linear, 5);
    case Easing::EaseOutQuint: {
      const double remaining = 1.0 - linear;
      return 1.0 - remaining * remaining * remaining * remaining * remaining;
    }
    case Easing::EaseInOutQuint:
      return linear < 0.5 ? 16.0 * std::pow(linear, 5) : 1.0 - std::pow(-2.0 * linear + 2.0, 5) / 2.0;

    // Expo
    case Easing::EaseInExpo:
      return linear <= 0.0 ? 0.0 : std::pow(2.0, 10.0 * linear - 10.0);
    case Easing::EaseOutExpo:
      return linear >= 1.0 ? 1.0 : 1.0 - std::pow(2.0, -10.0 * linear);
    case Easing::EaseInOutExpo:
      if (linear <= 0.0) {
        return 0.0;
      }
      if (linear >= 1.0) {
        return 1.0;
      }
      return linear < 0.5 ? std::pow(2.0, 20.0 * linear - 10.0) / 2.0
                          : (2.0 - std::pow(2.0, -20.0 * linear + 10.0)) / 2.0;

    // Circ
    case Easing::EaseInCirc:
      return 1.0 - std::sqrt(1.0 - std::pow(linear, 2));
    case Easing::EaseOutCirc:
      return std::sqrt(1.0 - std::pow(linear - 1.0, 2));
    case Easing::EaseInOutCirc:
      return linear < 0.5 ? (1.0 - std::sqrt(1.0 - std::pow(2.0 * linear, 2))) / 2.0
                          : (std::sqrt(1.0 - std::pow(-2.0 * linear + 2.0, 2)) + 1.0) / 2.0;

    // Back
    case Easing::EaseInBack: {
      constexpr double c1 = 1.70158;
      constexpr double c3 = c1 + 1.0;
      return c3 * linear * linear * linear - c1 * linear * linear;
    }
    case Easing::EaseOutBack: {
      constexpr double c1 = 1.70158;
      constexpr double c3 = c1 + 1.0;
      const double t = linear - 1.0;
      return 1.0 + c3 * t * t * t + c1 * t * t;
    }
    case Easing::EaseInOutBack: {
      constexpr double c1 = 1.70158 * 1.525;
      constexpr double c2 = c1 + 1.0;
      return linear < 0.5 ? (std::pow(2.0 * linear, 2) * ((c2 + 1.0) * 2.0 * linear - c2)) / 2.0
                          : (std::pow(2.0 * linear - 2.0, 2) * ((c2 + 1.0) * (2.0 * linear - 2.0) + c2) + 2.0) / 2.0;
    }

    // Elastic
    case Easing::EaseInElastic: {
      constexpr double c4 = (2.0 * kPi) / 3.0;
      if (linear <= 0.0) {
        return 0.0;
      }
      if (linear >= 1.0) {
        return 1.0;
      }
      return -std::pow(2.0, 10.0 * linear - 10.0) * std::sin((linear * 10.0 - 10.75) * c4);
    }
    case Easing::EaseOutElastic: {
      constexpr double c4 = (2.0 * kPi) / 3.0;
      if (linear <= 0.0) {
        return 0.0;
      }
      if (linear >= 1.0) {
        return 1.0;
      }
      return std::pow(2.0, -10.0 * linear) * std::sin((linear * 10.0 - 0.75) * c4) + 1.0;
    }
    case Easing::EaseInOutElastic: {
      constexpr double c5 = (2.0 * kPi) / 4.5;
      if (linear <= 0.0) {
        return 0.0;
      }
      if (linear >= 1.0) {
        return 1.0;
      }
      return linear < 0.5
          ? -(std::pow(2.0, 20.0 * linear - 10.0) * std::sin((20.0 * linear - 11.125) * c5)) / 2.0
          : (std::pow(2.0, -20.0 * linear + 10.0) * std::sin((20.0 * linear - 11.125) * c5)) / 2.0 + 1.0;
    }

    // Bounce
    case Easing::EaseInBounce: {
      const auto easeOutBounce = [](double t) {
        constexpr double n1 = 7.5625;
        constexpr double d1 = 2.75;
        if (t < 1.0 / d1) {
          return n1 * t * t;
        }
        if (t < 2.0 / d1) {
          t -= 1.5 / d1;
          return n1 * t * t + 0.75;
        }
        if (t < 2.5 / d1) {
          t -= 2.25 / d1;
          return n1 * t * t + 0.9375;
        }
        t -= 2.625 / d1;
        return n1 * t * t + 0.984375;
      };
      return 1.0 - easeOutBounce(1.0 - linear);
    }
    case Easing::EaseOutBounce: {
      constexpr double n1 = 7.5625;
      constexpr double d1 = 2.75;
      double t = linear;
      if (t < 1.0 / d1) {
        return n1 * t * t;
      }
      if (t < 2.0 / d1) {
        t -= 1.5 / d1;
        return n1 * t * t + 0.75;
      }
      if (t < 2.5 / d1) {
        t -= 2.25 / d1;
        return n1 * t * t + 0.9375;
      }
      t -= 2.625 / d1;
      return n1 * t * t + 0.984375;
    }
    case Easing::EaseInOutBounce: {
      const auto easeOutBounce = [](double t) {
        constexpr double n1 = 7.5625;
        constexpr double d1 = 2.75;
        if (t < 1.0 / d1) {
          return n1 * t * t;
        }
        if (t < 2.0 / d1) {
          t -= 1.5 / d1;
          return n1 * t * t + 0.75;
        }
        if (t < 2.5 / d1) {
          t -= 2.25 / d1;
          return n1 * t * t + 0.9375;
        }
        t -= 2.625 / d1;
        return n1 * t * t + 0.984375;
      };
      return linear < 0.5 ? (1.0 - easeOutBounce(1.0 - 2.0 * linear)) / 2.0
                          : (1.0 + easeOutBounce(2.0 * linear - 1.0)) / 2.0;
    }

    case Easing::Snappy:
      return solveCubicBezier(0.05, 0.9, 0.1, 1.05, linear);

    case Easing::CustomBezier:
      return solveCubicBezier(curve.bezier.x1, curve.bezier.y1, curve.bezier.x2, curve.bezier.y2, linear);

    case Easing::Spring: {
      // Physics over the spring's own settle time, so damping shapes the response while stiffness and mass set how
      // long it takes. Normalized time would cancel both, leaving damping as the only observable parameter.
      const double seconds = linear * static_cast<double>(springDurationMs(curve.spring)) / 1000.0;
      return solveSpringPhysics(0.0, 1.0, 0.0, seconds, curve.spring);
    }
    }

    return linear;
  }

  MonotonicEasing::MonotonicEasing() { reset(AnimationCurve{.easing = Easing::Linear}); }

  MonotonicEasing::MonotonicEasing(const AnimationCurve& curve) { reset(curve); }

  void MonotonicEasing::reset(const AnimationCurve& curve) {
    m_curve = curve;
    std::array<double, kSampleCount + 1> raw{};
    raw.front() = 0.0;
    raw.back() = 1.0;

    m_direct = true;
    double previous = raw.front();
    for (std::size_t i = 1; i < kSampleCount; ++i) {
      const double progress = static_cast<double>(i) / static_cast<double>(kSampleCount);
      const double current = evaluateCurve(curve, progress);
      if (!std::isfinite(current)) {
        m_curve = AnimationCurve{.easing = Easing::Linear};
        m_direct = true;
        return;
      }
      raw[i] = current;
      if (current < previous || current < 0.0 || current > 1.0) {
        m_direct = false;
      }
      previous = current;
    }
    if (raw.back() < previous) {
      m_direct = false;
    }
    if (m_direct) {
      return;
    }

    // Scale before subtracting so even extreme, but finite, custom Bezier control points cannot overflow a delta.
    double scale = 1.0;
    for (const double sample : raw) {
      scale = std::max(scale, std::abs(sample));
    }
    m_progress.front() = 0.0;
    for (std::size_t i = 1; i <= kSampleCount; ++i) {
      const double delta = std::abs(raw[i] / scale - raw[i - 1] / scale);
      m_progress[i] = m_progress[i - 1] + delta;
    }
    const double total = m_progress.back();
    if (!std::isfinite(total) || total <= std::numeric_limits<double>::epsilon()) {
      m_curve = AnimationCurve{.easing = Easing::Linear};
      m_direct = true;
      return;
    }
    for (double& sample : m_progress) {
      sample /= total;
    }
    m_progress.front() = 0.0;
    m_progress.back() = 1.0;
  }

  double MonotonicEasing::value(double linearProgress) const {
    const double linear = std::clamp(linearProgress, 0.0, 1.0);
    if (linear <= 0.0) {
      return 0.0;
    }
    if (linear >= 1.0) {
      return 1.0;
    }
    if (m_direct) {
      return std::clamp(evaluateCurve(m_curve, linear), 0.0, 1.0);
    }

    const double sample = linear * static_cast<double>(kSampleCount);
    const auto lower = static_cast<std::size_t>(sample);
    const double fraction = sample - static_cast<double>(lower);
    return std::lerp(m_progress[lower], m_progress[lower + 1], fraction);
  }

  Spring::Spring(double from, double to, const SpringConfig& config, double velocity)
      : m_config(config), m_from(from), m_to(to), m_value(from), m_velocity(velocity), m_startVelocity(velocity),
        m_settled(from == to && velocity == 0.0) {}

  void Spring::retarget(double to) {
    m_from = m_value;
    m_startVelocity = m_velocity;
    m_to = to;
    m_elapsed = 0.0;
    m_settled = m_from == m_to && m_velocity == 0.0;
  }

  double Spring::step(double seconds) {
    if (m_settled) {
      return m_value;
    }
    m_elapsed += std::max(seconds, 0.0);
    double velocity = 0.0;
    m_value = solveSpringPhysics(m_from, m_to, m_startVelocity, m_elapsed, m_config, &velocity);
    m_velocity = velocity;
    if (m_value == m_to && m_velocity == 0.0) {
      m_settled = true;
    }
    return m_value;
  }

} // namespace desk::motion
