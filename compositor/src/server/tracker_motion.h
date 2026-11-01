#pragma once

#include "input/swipe_tracker.h"
#include "motion/desk_motion.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace andromeda {

  // The position of one dsk_tracker_v1: driven by gesture input while interacting, then by a spring that carries the
  // release velocity to a snap point (inertia), then at rest (idle). Pure logic: no Wayland, no scene.
  class TrackerMotion {
  public:
    enum class Phase : uint8_t { Idle, Interacting, Inertia };

    struct Event {
      enum class Kind : uint8_t { Interacting, Inertia, Idle } kind;
      uint32_t timeMs;
      double position; // Inertia: the rest target; Idle: the rest position
    };

    // Past either end the position rubber-bands: extra finger travel approaches this many px without reaching it.
    static constexpr double kOverscrollPx = 40.0;

    void setRange(double minimum, double maximum);
    void addSnap(double position);

    [[nodiscard]] double position() const { return m_position; }
    [[nodiscard]] Phase phase() const { return m_phase; }

    void begin(uint32_t timeMs);
    void update(double delta, uint32_t timeMs);
    // Projects the release to where it would coast to a stop, then settles on the nearest snap (or the range edge).
    void end(uint32_t timeMs);
    // Drives the position without input, for example a click that opens a sheet. A running gesture is not interrupted.
    void moveTo(double target, const desk::motion::SpringConfig& spring, uint32_t timeMs);
    // Moves at once and rests there: a move_to with curve none, or any move under reduce motion.
    void jumpTo(double target, uint32_t timeMs);
    // Advances inertia to `nowMs`. Returns true when the position changed.
    bool tick(uint32_t nowMs);
    std::vector<Event> takeEvents();

  private:
    [[nodiscard]] double rubberBand(double position) const;
    [[nodiscard]] double restTarget(double projected) const;
    void startInertia(double target, double velocity, const desk::motion::SpringConfig& spring, uint32_t timeMs);

    double m_minimum = 0.0;
    double m_maximum = 0.0;
    std::vector<double> m_snaps;
    Phase m_phase = Phase::Idle;
    double m_position = 0.0;
    double m_origin = 0.0;
    SwipeTracker m_tracker;
    desk::motion::Spring m_spring;
    std::optional<uint32_t> m_lastTickMs;
    std::vector<Event> m_events;
  };

} // namespace andromeda
