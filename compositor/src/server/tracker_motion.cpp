#include "server/tracker_motion.h"

#include "design_tokens.h"
#include "input/gesture_physics.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace andromeda {

  namespace {
    desk::motion::SpringConfig releaseSpring() {
      return {.damping = Tokens::springSnappy.damping, .stiffness = Tokens::springSnappy.stiffness};
    }
  } // namespace

  void TrackerMotion::setRange(double minimum, double maximum) {
    m_minimum = std::min(minimum, maximum);
    m_maximum = std::max(minimum, maximum);
  }

  void TrackerMotion::addSnap(double position) { m_snaps.push_back(position); }

  double TrackerMotion::rubberBand(double position) const {
    return GesturePhysics::rubberBand(position, m_minimum, m_maximum, kOverscrollPx);
  }

  double TrackerMotion::restTarget(double projected) const {
    const double clamped = std::clamp(projected, m_minimum, m_maximum);
    if (m_snaps.empty()) {
      return clamped;
    }
    return *std::ranges::min_element(m_snaps, {}, [&](double snap) { return std::abs(snap - clamped); });
  }

  void TrackerMotion::begin(uint32_t timeMs) {
    m_tracker.reset();
    m_origin = m_position;
    m_phase = Phase::Interacting;
    m_events.push_back({.kind = Event::Kind::Interacting, .timeMs = timeMs, .position = m_position});
  }

  void TrackerMotion::update(double delta, uint32_t timeMs) {
    if (m_phase != Phase::Interacting) {
      return;
    }
    m_tracker.push(delta, timeMs);
    m_position = rubberBand(m_origin + m_tracker.pos());
  }

  void TrackerMotion::end(uint32_t timeMs) {
    if (m_phase != Phase::Interacting) {
      return;
    }
    // A zero sample at the release lets idle time before letting go bleed off the speed.
    m_tracker.push(0.0, timeMs);
    const double target = restTarget(m_origin + m_tracker.projectedEndPos());
    const double velocity = m_tracker.velocity()
        * GesturePhysics::rubberBandDerivative(m_origin + m_tracker.pos(), m_minimum, m_maximum, kOverscrollPx);
    startInertia(target, velocity, releaseSpring(), timeMs);
  }

  void TrackerMotion::moveTo(double target, const desk::motion::SpringConfig& spring, uint32_t timeMs) {
    if (m_phase == Phase::Interacting) {
      return;
    }
    const double velocity = m_phase == Phase::Inertia ? m_spring.velocity() : 0.0;
    startInertia(std::clamp(target, m_minimum, m_maximum), velocity, spring, timeMs);
  }

  void TrackerMotion::jumpTo(double target, uint32_t timeMs) {
    if (m_phase == Phase::Interacting) {
      return;
    }
    m_position = std::clamp(target, m_minimum, m_maximum);
    m_phase = Phase::Idle;
    m_events.push_back({.kind = Event::Kind::Idle, .timeMs = timeMs, .position = m_position});
  }

  void TrackerMotion::startInertia(
      double target, double velocity, const desk::motion::SpringConfig& spring, uint32_t timeMs
  ) {
    m_spring = desk::motion::Spring(m_position, target, spring, velocity);
    m_phase = Phase::Inertia;
    m_lastTickMs = timeMs;
    m_events.push_back({.kind = Event::Kind::Inertia, .timeMs = timeMs, .position = target});
  }

  bool TrackerMotion::tick(uint32_t nowMs) {
    if (m_phase != Phase::Inertia) {
      return false;
    }
    const double dt = m_lastTickMs ? static_cast<double>(nowMs - *m_lastTickMs) / 1000.0 : 0.0;
    m_lastTickMs = nowMs;
    m_position = m_spring.step(dt);
    if (m_spring.settled()) {
      m_position = m_spring.target();
      m_phase = Phase::Idle;
      m_events.push_back({.kind = Event::Kind::Idle, .timeMs = nowMs, .position = m_position});
    }
    return true;
  }

  std::vector<TrackerMotion::Event> TrackerMotion::takeEvents() { return std::exchange(m_events, {}); }

} // namespace andromeda
