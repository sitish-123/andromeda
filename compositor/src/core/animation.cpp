#include "core/animation.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <mutex>
#include <numbers>
#include <shared_mutex>
#include <sys/random.h>
#include <unistd.h>
#include <unordered_map>

namespace andromeda {

  uint64_t nextAnimationTransitionId() {
    static std::atomic<uint64_t> nextId{1};
    uint64_t id = nextId.fetch_add(1, std::memory_order_relaxed);
    if (id == 0) {
      id = nextId.fetch_add(1, std::memory_order_relaxed);
    }
    return id;
  }

  namespace {

    [[nodiscard]] uint64_t mixRandom(uint64_t value) {
      value = (value ^ (value >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
      value = (value ^ (value >> 27)) * UINT64_C(0x94d049bb133111eb);
      return value ^ (value >> 31);
    }

    [[nodiscard]] uint64_t animationRandomSalt() {
      uint64_t salt = 0;
      if (getrandom(&salt, sizeof(salt), GRND_NONBLOCK) != static_cast<ssize_t>(sizeof(salt))) {
        salt = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count())
            ^ static_cast<uint64_t>(getpid())
            ^ reinterpret_cast<uintptr_t>(&salt);
      }
      return mixRandom(salt);
    }

    [[nodiscard]] std::string normalizeName(std::string_view name) {
      std::string out;
      out.reserve(name.size());
      for (char ch : name) {
        if (ch != '_' && ch != '-' && ch != ' ') {
          out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
        }
      }
      return out;
    }

    class CurveRegistryImpl {
    public:
      CurveRegistryImpl() { initDefaults(); }

      void reset() {
        std::unique_lock lock(m_mutex);
        m_curves.clear();
        populateDefaults();
      }

      void registerCurve(std::string_view name, const AnimationCurve& curve) {
        std::unique_lock lock(m_mutex);
        m_curves[normalizeName(name)] = curve;
      }

      [[nodiscard]] std::optional<AnimationCurve> lookup(std::string_view name) const {
        std::shared_lock lock(m_mutex);
        const auto it = m_curves.find(normalizeName(name));
        if (it != m_curves.end()) {
          return it->second;
        }
        return std::nullopt;
      }

      bool unregisterCurve(std::string_view name) {
        std::unique_lock lock(m_mutex);
        return m_curves.erase(normalizeName(name)) > 0;
      }

      [[nodiscard]] bool has(std::string_view name) const {
        std::shared_lock lock(m_mutex);
        return m_curves.contains(normalizeName(name));
      }

      [[nodiscard]] std::optional<AnimationCurve> parse(std::string_view str) const {
        std::string value(str);
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
          value.erase(value.begin());
        }
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
          value.pop_back();
        }
        if (value.empty()) {
          return std::nullopt;
        }
        if (auto curve = lookup(value)) {
          return curve;
        }

        const auto fullyConsumed = [](const std::string& input, int consumed) {
          return consumed > 0
              && std::ranges::all_of(std::string_view(input).substr(static_cast<size_t>(consumed)), [](char ch) {
                   return std::isspace(static_cast<unsigned char>(ch));
                 });
        };

        double x1 = 0.0;
        double y1 = 0.0;
        double x2 = 0.0;
        double y2 = 0.0;
        int consumed = 0;
        if (std::sscanf(value.c_str(), " %lf , %lf , %lf , %lf %n", &x1, &y1, &x2, &y2, &consumed) == 4
            && fullyConsumed(value, consumed)
            && std::isfinite(x1)
            && std::isfinite(y1)
            && std::isfinite(x2)
            && std::isfinite(y2)
            && x1 >= 0.0
            && x1 <= 1.0
            && x2 >= 0.0
            && x2 <= 1.0) {
          return AnimationCurve{.easing = Easing::CustomBezier, .bezier = {x1, y1, x2, y2}};
        }

        constexpr std::string_view kSpringPrefix = "spring:";
        if (!value.starts_with(kSpringPrefix)) {
          return std::nullopt;
        }
        const std::string parameters = value.substr(kSpringPrefix.size());
        double damping = 0.0;
        double stiffness = 0.0;
        consumed = 0;
        if (std::sscanf(parameters.c_str(), " %lf , %lf %n", &damping, &stiffness, &consumed) != 2
            || !fullyConsumed(parameters, consumed)
            || !std::isfinite(damping)
            || !std::isfinite(stiffness)
            || damping < 0.01
            || damping > 5.0
            || stiffness < 1.0
            || stiffness > 10000.0) {
          return std::nullopt;
        }
        return AnimationCurve{
            .easing = Easing::Spring, .spring = {.damping = damping, .stiffness = stiffness, .mass = 1.0}
        };
      }

    private:
      void initDefaults() {
        std::unique_lock lock(m_mutex);
        populateDefaults();
      }

      void populateDefaults() {
        // Standard Easing Presets
        m_curves["linear"] = AnimationCurve{.easing = Easing::Linear};

        // Sine
        m_curves["easeinsine"] = AnimationCurve{.easing = Easing::EaseInSine};
        m_curves["easeoutsine"] = AnimationCurve{.easing = Easing::EaseOutSine};
        m_curves["easeinoutsine"] = AnimationCurve{.easing = Easing::EaseInOutSine};

        // Quad
        m_curves["easeinquad"] = AnimationCurve{.easing = Easing::EaseInQuad};
        m_curves["easeoutquad"] = AnimationCurve{.easing = Easing::EaseOutQuad};
        m_curves["easeinoutquad"] = AnimationCurve{.easing = Easing::EaseInOutQuad};
        m_curves["quad"] = AnimationCurve{.easing = Easing::EaseOutQuad};

        // Cubic
        m_curves["easeincubic"] = AnimationCurve{.easing = Easing::EaseInCubic};
        m_curves["easeoutcubic"] = AnimationCurve{.easing = Easing::EaseOutCubic};
        m_curves["easeinoutcubic"] = AnimationCurve{.easing = Easing::EaseInOutCubic};
        m_curves["cubic"] = AnimationCurve{.easing = Easing::EaseOutCubic};
        m_curves["ease"] = AnimationCurve{.easing = Easing::EaseOutCubic};
        m_curves["easein"] = AnimationCurve{.easing = Easing::EaseInCubic};
        m_curves["easeout"] = AnimationCurve{.easing = Easing::EaseOutCubic};
        m_curves["easeinout"] = AnimationCurve{.easing = Easing::EaseInOutCubic};

        // Quart
        m_curves["easeinquart"] = AnimationCurve{.easing = Easing::EaseInQuart};
        m_curves["easeoutquart"] = AnimationCurve{.easing = Easing::EaseOutQuart};
        m_curves["easeinoutquart"] = AnimationCurve{.easing = Easing::EaseInOutQuart};
        m_curves["quart"] = AnimationCurve{.easing = Easing::EaseOutQuart};

        // Quint
        m_curves["easeinquint"] = AnimationCurve{.easing = Easing::EaseInQuint};
        m_curves["easeoutquint"] = AnimationCurve{.easing = Easing::EaseOutQuint};
        m_curves["easeinoutquint"] = AnimationCurve{.easing = Easing::EaseInOutQuint};
        m_curves["quint"] = AnimationCurve{.easing = Easing::EaseOutQuint};

        // Expo
        m_curves["easeinexpo"] = AnimationCurve{.easing = Easing::EaseInExpo};
        m_curves["easeoutexpo"] = AnimationCurve{.easing = Easing::EaseOutExpo};
        m_curves["easeinoutexpo"] = AnimationCurve{.easing = Easing::EaseInOutExpo};
        m_curves["expo"] = AnimationCurve{.easing = Easing::EaseOutExpo};

        // Circ
        m_curves["easeincirc"] = AnimationCurve{.easing = Easing::EaseInCirc};
        m_curves["easeoutcirc"] = AnimationCurve{.easing = Easing::EaseOutCirc};
        m_curves["easeinoutcirc"] = AnimationCurve{.easing = Easing::EaseInOutCirc};
        m_curves["circ"] = AnimationCurve{.easing = Easing::EaseOutCirc};

        // Back
        m_curves["easeinback"] = AnimationCurve{.easing = Easing::EaseInBack};
        m_curves["easeoutback"] = AnimationCurve{.easing = Easing::EaseOutBack};
        m_curves["easeinoutback"] = AnimationCurve{.easing = Easing::EaseInOutBack};
        m_curves["back"] = AnimationCurve{.easing = Easing::EaseOutBack};
        m_curves["overshoot"] = AnimationCurve{.easing = Easing::EaseOutBack};

        // Elastic
        m_curves["easeinelastic"] = AnimationCurve{.easing = Easing::EaseInElastic};
        m_curves["easeoutelastic"] = AnimationCurve{.easing = Easing::EaseOutElastic};
        m_curves["easeinoutelastic"] = AnimationCurve{.easing = Easing::EaseInOutElastic};
        m_curves["elastic"] = AnimationCurve{.easing = Easing::EaseOutElastic};

        // Bounce
        m_curves["easeinbounce"] = AnimationCurve{.easing = Easing::EaseInBounce};
        m_curves["easeoutbounce"] = AnimationCurve{.easing = Easing::EaseOutBounce};
        m_curves["easeinoutbounce"] = AnimationCurve{.easing = Easing::EaseInOutBounce};
        m_curves["bounce"] = AnimationCurve{.easing = Easing::EaseOutBounce};

        // Default built-in animation curves
        m_curves["snappy"] = AnimationCurve{.easing = Easing::Snappy};
        m_curves["default"] = AnimationCurve{.easing = Easing::Snappy};
        m_curves["easeoutquint"] = AnimationCurve{.easing = Easing::CustomBezier, .bezier = {0.23, 1.0, 0.32, 1.0}};

        // Standard Named Springs
        m_curves["defaultspring"] =
            AnimationCurve{.easing = Easing::Spring, .spring = {.damping = 0.75, .stiffness = 100.0, .mass = 1.0}};
        m_curves["bouncy"] =
            AnimationCurve{.easing = Easing::Spring, .spring = {.damping = 0.5, .stiffness = 120.0, .mass = 1.0}};
        m_curves["smooth"] =
            AnimationCurve{.easing = Easing::Spring, .spring = {.damping = 0.9, .stiffness = 90.0, .mass = 1.0}};
        m_curves["stiff"] =
            AnimationCurve{.easing = Easing::Spring, .spring = {.damping = 0.8, .stiffness = 200.0, .mass = 1.0}};
      }

      mutable std::shared_mutex m_mutex;
      std::unordered_map<std::string, AnimationCurve> m_curves;
    };

    CurveRegistryImpl& registryImpl() {
      static CurveRegistryImpl impl;
      return impl;
    }
  } // namespace

  [[nodiscard]] AnimationTransition beginAnimationTransition() {
    static const uint64_t salt = animationRandomSalt();
    const uint64_t id = nextAnimationTransitionId();
    AnimationTransition transition{.id = id, .seed = {}};
    for (std::size_t channel = 0; channel < transition.seed.size(); ++channel) {
      const uint64_t value = mixRandom(salt ^ mixRandom(id + UINT64_C(0x9e3779b97f4a7c15) * (channel + 1)));
      transition.seed[channel] = static_cast<float>(value >> 40) * (1.0F / 16777216.0F);
    }
    return transition;
  }


  // CurveRegistry methods
  void CurveRegistry::registerCurve(std::string_view name, const AnimationCurve& curve) {
    registryImpl().registerCurve(name, curve);
  }

  void CurveRegistry::registerBezier(std::string_view name, double x1, double y1, double x2, double y2) {
    AnimationCurve c;
    c.easing = Easing::CustomBezier;
    c.bezier = {x1, y1, x2, y2};
    registryImpl().registerCurve(name, c);
  }

  void CurveRegistry::registerSpring(std::string_view name, double damping, double stiffness, double mass) {
    AnimationCurve c;
    c.easing = Easing::Spring;
    c.spring = {damping, stiffness, mass};
    registryImpl().registerCurve(name, c);
  }

  std::optional<AnimationCurve> CurveRegistry::lookup(std::string_view name) { return registryImpl().lookup(name); }

  AnimationCurve CurveRegistry::get(std::string_view name, const AnimationCurve& fallback) {
    return lookup(name).value_or(fallback);
  }

  bool CurveRegistry::has(std::string_view name) { return registryImpl().has(name); }

  bool CurveRegistry::unregisterCurve(std::string_view name) { return registryImpl().unregisterCurve(name); }

  void CurveRegistry::resetToDefaults() { registryImpl().reset(); }

  std::optional<AnimationCurve> CurveRegistry::parse(std::string_view str) { return registryImpl().parse(str); }

  // AnimatedValue
  void AnimatedValue::snap(double value) {
    m_from = value;
    m_target = value;
    m_current = value;
    m_velocity = 0.0;
    m_initialVelocity = 0.0;
    m_progress = 1.0;
    m_startMsec = 0;
    m_animating = false;
    m_physics = false;
  }

  void AnimatedValue::retarget(double to, int durationMs, Easing easing) {
    retarget(to, durationMs, AnimationCurve{.easing = easing});
  }

  void AnimatedValue::retarget(double to, int durationMs, const AnimationCurve& curve) {
    // A spring interrupted mid-flight continues from its current speed instead of restarting from rest, so a new target
    // bends the motion rather than stopping it for a frame.
    if (curve.easing == Easing::Spring && m_animating && m_velocity != 0.0 && std::isfinite(m_velocity)) {
      settleSpring(to, curve.spring, m_velocity);
      return;
    }
    const AnimationTransition transition = beginAnimationTransition();
    m_transitionId = transition.id;
    m_shaderSeed = transition.seed;
    m_from = m_current;
    m_target = to;
    m_durationMsec = curve.easing == Easing::Spring ? static_cast<uint64_t>(springDurationMs(curve.spring))
                                                    : static_cast<uint64_t>(std::max(1, durationMs));
    m_curve = curve;
    m_startMsec = 0;
    m_progress = 0.0;
    m_animating = true;
    m_physics = false;
    m_initialVelocity = 0.0;
    m_velocity = 0.0;
  }

  void AnimatedValue::retarget(double to, int durationMs, std::string_view curveName) {
    retarget(to, durationMs, CurveRegistry::get(curveName));
  }

  void AnimatedValue::retargetBezier(double to, int durationMs, double x1, double y1, double x2, double y2) {
    AnimationCurve c;
    c.easing = Easing::CustomBezier;
    c.bezier = {x1, y1, x2, y2};
    retarget(to, durationMs, c);
  }

  void AnimatedValue::retargetSpring(double to, double damping, double stiffness) {
    AnimationCurve c;
    c.easing = Easing::Spring;
    c.spring = {damping, stiffness, 1.0};
    retarget(to, 0, c);
  }

  void AnimatedValue::settleSpring(double to, const SpringConfig& spring, double initialVelocity) {
    const AnimationTransition transition = beginAnimationTransition();
    m_transitionId = transition.id;
    m_shaderSeed = transition.seed;
    m_from = m_current;
    m_target = to;
    m_curve = AnimationCurve{.easing = Easing::Spring, .spring = spring};
    m_initialVelocity = initialVelocity;
    m_velocity = initialVelocity;
    m_startMsec = 0;
    m_progress = 0.0;
    m_animating = true;
    m_physics = true;
  }

  bool AnimatedValue::finishSpringTail(double pixelsPerUnit) {
    if (!m_animating || !m_physics || !std::isfinite(pixelsPerUnit) || pixelsPerUnit <= 0.0) {
      return false;
    }
    const double targetPixels = m_target * pixelsPerUnit;
    if (!std::isfinite(targetPixels)) {
      return false;
    }
    const double roundedTarget = std::round(targetPixels);
    const double roundingMargin = 0.5 - std::abs(targetPixels - roundedTarget);
    const double remaining = springDisplacementBound(m_current, m_target, m_velocity, m_curve.spring) * pixelsPerUnit;
    if (roundingMargin <= 0.0 || remaining >= roundingMargin) {
      return false;
    }
    snap(m_target);
    return true;
  }

  void AnimatedValue::translate(double delta) {
    m_from += delta;
    m_target += delta;
    m_current += delta;
  }

  double AnimatedValue::progress() const { return m_progress; }

  bool AnimatedValue::tick(double nowMsec) {
    if (!m_animating) {
      return false;
    }
    if (m_startMsec == 0) {
      m_startMsec = nowMsec;
    }

    const double elapsed = std::max(0.0, nowMsec - m_startMsec);

    if (m_physics) {
      double velocity = 0.0;
      m_current = solveSpringPhysics(m_from, m_target, m_initialVelocity, elapsed / 1000.0, m_curve.spring, &velocity);
      m_velocity = velocity;
      const double span = m_target - m_from;
      m_progress = span != 0.0 ? std::clamp((m_current - m_from) / span, 0.0, 1.0) : 1.0;
      if (m_current == m_target && velocity == 0.0) {
        m_progress = 1.0;
        m_animating = false;
      }
      return true;
    }

    const double linear = std::clamp(elapsed / static_cast<double>(m_durationMsec), 0.0, 1.0);
    m_progress = linear;

    if (linear >= 1.0) {
      m_current = m_target;
      m_velocity = 0.0;
      m_animating = false;
      return true;
    }

    const double eased = applyEasing(m_curve, linear);
    m_current = m_from + (m_target - m_from) * eased;

    // The curve's slope at this instant, in units per second, measured over one millisecond of curve time.
    const double stepMs = 1.0;
    const double before = std::max(0.0, linear - stepMs / static_cast<double>(m_durationMsec));
    const double sloped = linear - before;
    m_velocity = sloped > 0.0
        ? (m_target - m_from) * (eased - applyEasing(m_curve, before)) / (sloped * m_durationMsec / 1000.0)
        : 0.0;

    return true;
  }

  // AnimatedColor
  void AnimatedColor::snap(const std::array<float, 4>& color) {
    m_from = color;
    m_target = color;
    m_current = color;
    m_fromOkLab = srgbToOkLab(color);
    m_targetOkLab = m_fromOkLab;
    m_progress = 1.0;
    m_startMsec = 0;
    m_animating = false;
  }

  void AnimatedColor::snap(float r, float g, float b, float a) { snap(std::array<float, 4>{r, g, b, a}); }

  void AnimatedColor::retarget(const std::array<float, 4>& to, int durationMs, Easing easing) {
    retarget(to, durationMs, AnimationCurve{.easing = easing});
  }

  void AnimatedColor::retarget(const std::array<float, 4>& to, int durationMs, const AnimationCurve& curve) {
    const AnimationTransition transition = beginAnimationTransition();
    m_transitionId = transition.id;
    m_shaderSeed = transition.seed;
    m_from = m_current;
    m_target = to;
    m_fromOkLab = srgbToOkLab(m_from);
    m_targetOkLab = srgbToOkLab(m_target);
    m_durationMsec = curve.easing == Easing::Spring ? static_cast<uint64_t>(springDurationMs(curve.spring))
                                                    : static_cast<uint64_t>(std::max(1, durationMs));
    m_curve = curve;
    m_startMsec = 0;
    m_progress = 0.0;
    m_animating = true;
  }

  void AnimatedColor::retarget(const std::array<float, 4>& to, int durationMs, std::string_view curveName) {
    retarget(to, durationMs, CurveRegistry::get(curveName));
  }

  void AnimatedColor::retarget(float r, float g, float b, float a, int durationMs, const AnimationCurve& curve) {
    retarget(std::array<float, 4>{r, g, b, a}, durationMs, curve);
  }

  void AnimatedColor::retargetBezier(
      const std::array<float, 4>& to, int durationMs, double x1, double y1, double x2, double y2
  ) {
    AnimationCurve c;
    c.easing = Easing::CustomBezier;
    c.bezier = {x1, y1, x2, y2};
    retarget(to, durationMs, c);
  }

  void AnimatedColor::retargetSpring(const std::array<float, 4>& to, double damping, double stiffness) {
    AnimationCurve c;
    c.easing = Easing::Spring;
    c.spring = {damping, stiffness, 1.0};
    retarget(to, 0, c);
  }

  double AnimatedColor::progress() const { return m_progress; }

  bool AnimatedColor::tick(double nowMsec) {
    if (!m_animating) {
      return false;
    }
    if (m_startMsec == 0) {
      m_startMsec = nowMsec;
    }

    const double elapsed = std::max(0.0, nowMsec - m_startMsec);
    const double linear = std::clamp(elapsed / static_cast<double>(m_durationMsec), 0.0, 1.0);
    m_progress = linear;

    if (linear >= 1.0) {
      m_current = m_target;
      m_animating = false;
      return true;
    }

    const auto eased = static_cast<float>(applyEasing(m_curve, linear));
    const OkLab interpolated = interpolateOkLab(m_fromOkLab, m_targetOkLab, eased);
    const float alpha = std::lerp(m_from[3], m_target[3], eased);
    m_current = okLabToSrgb(interpolated, alpha);

    return true;
  }

  void AnimatedColor::current(float out[4]) const {
    out[0] = m_current[0];
    out[1] = m_current[1];
    out[2] = m_current[2];
    out[3] = m_current[3];
  }

} // namespace andromeda
