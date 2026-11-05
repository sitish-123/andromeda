#pragma once

#include "motion/desk_motion.h"

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace andromeda {

  // The animated properties of one dsk_surface_v1. Pure logic: no Wayland, no scene.
  class SurfaceMotion {
  public:
    // Values match dsk_surface_v1.property.
    enum class Property : uint8_t {
      Translate, Scale, Opacity, Clip, Radius, Elevation, Liquid, Stroke, Tint, Join, Fill, FillDense
    };
    // Values match dsk_surface_v1.curve.
    enum class Curve : uint8_t {
      None, SpringStandard, SpringSnappy, SpringBouncy, EaseFast, EaseNormal, EaseSlow, SpringOpen, SpringClose,
      SpringIslandExpand, SpringIslandCollapse, SpringPage, SpringPress, SpringRelease, SpringEffects, SpringEffectsFast
    };
    enum class Reason : uint8_t { Finished, Retargeted, Cancelled };
    // Values match dsk_shape_v1.wave.
    enum class Wave : uint8_t { Sine, Noise };
    struct LoopSpec {
      Wave wave = Wave::Sine;
      uint32_t periodMs = 1000;
      uint32_t seed = 0;
    };
    static constexpr std::size_t kPropertyCount = 12;

    using Value = std::array<double, 4>;

    struct Event {
      enum class Kind : uint8_t { Started, Done } kind;
      uint32_t serial;
      Reason reason;
    };

    static bool isSpring(Curve curve);
    static desk::motion::SpringConfig springConfig(Curve curve);

    SurfaceMotion();

    [[nodiscard]] const Value& value(Property property) const;
    // Where the property ends: the running animation's target, else the current value.
    [[nodiscard]] const Value& target(Property property) const;
    [[nodiscard]] bool animating() const;
    // A spring runs: it needs frames until it settles. Loops need frames only while their surface shows.
    [[nodiscard]] bool settling() const;
    [[nodiscard]] bool looping() const;

    // Jumps; a running animation on the property ends as Cancelled.
    void set(Property property, const Value& value);
    // Starts from the current value; a spring already running on the property keeps its velocity.
    void animate(uint32_t serial, Property property, Curve curve, const Value& to);
    // Repeats between the current value and `high` until a set, animate or loop on the property replaces it.
    void loop(uint32_t serial, Property property, Wave wave, uint32_t periodMs, uint32_t seed, const Value& high);
    // Advances to `nowMs` (CLOCK_MONOTONIC). The first tick after animate() is the animation's start frame.
    // Returns true when a value changed.
    bool tick(uint32_t nowMs);
    // Started and Done events since the last call, in order.
    std::vector<Event> takeEvents();

  private:
    struct Track {
      Curve curve = Curve::None;
      uint32_t serial = 0;
      bool running = false;
      bool started = false;
      // A retarget keeps moving on its start frame; a fresh animation shows its start value first.
      bool stepOnStart = false;
      Value to{};
      std::array<desk::motion::Spring, 4> springs{};
      // A loop: value = low + (to - low) * wave(elapsed / period), from the start frame on.
      bool loop = false;
      Wave wave = Wave::Sine;
      uint32_t periodMs = 1000;
      uint32_t seed = 0;
      uint32_t startMs = 0;
      Value low{};
    };

    void finish(Track& track, Reason reason);

    std::array<Value, kPropertyCount> m_values{};
    std::array<Track, kPropertyCount> m_tracks{};
    std::optional<uint32_t> m_lastTickMs;
    std::vector<Event> m_events;
  };

} // namespace andromeda
