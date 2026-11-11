#pragma once

#include "motion/desk_motion.h"
#include "shell/live_activity/island_morph.h"

#include <array>
#include <cstdint>
#include <vector>

// IslandMorph and the mirror of the compositor's clip springs (x, y, w, h, radius) and the face fade, on the clock and
// drivers of a Host:
//   std::uint32_t nowMs();       monotonic milliseconds
//   Driver startDriver();        a capped timer: step() each tick, driverDone(id) when it ends on its own
//   void cancel(Driver);         stops a driver at once (not from inside its own tick); zero is a no-op
//   void reapSoon();             calls reap() on a later turn
//   void redraw();
//   void reveal(float);          the fade value while the morph runs
//   void settle();               the morph ended at its target
template <typename Face, typename Rect, typename Driver> class IslandMorphDriver {
public:
  using Morph = IslandMorph<Face, Rect, Driver>;
  using Spring = desk::motion::Spring;
  using SpringConfig = desk::motion::SpringConfig;
  using Shape = std::array<double, 5>;
  static constexpr std::int32_t kStartWaitMs = 500;

  explicit IslandMorphDriver(Face face) : m_morph(face) {}

  [[nodiscard]] typename Morph::Phase phase() const noexcept { return m_morph.phase(); }
  [[nodiscard]] bool settled() const noexcept { return m_morph.settled(); }
  [[nodiscard]] Face shown() const noexcept { return m_morph.shown(); }
  [[nodiscard]] Face from() const noexcept { return m_morph.from(); }
  [[nodiscard]] const Rect& fromRect() const noexcept { return m_morph.fromRect(); }
  [[nodiscard]] std::uint32_t gen() const noexcept { return m_morph.gen(); }
  [[nodiscard]] Driver driver() const noexcept { return m_morph.driver(); }
  [[nodiscard]] const std::array<Spring, 5>& shape() const noexcept { return m_shape; }
  typename Morph::Result apply(const typename Morph::Event& event) { return m_morph.apply(event); }

  // A face change on a new driver; the fade holds until startShape and `started` or startFadeNow.
  template <typename Host> typename Morph::Result begin(Host& host, Face face, const Rect& fromRect, const SpringConfig& fade) {
    m_fade = Spring(0.0, 1.0, fade);
    m_fadeSec = 0.0;
    const auto begun = m_morph.apply(typename Morph::Begin{face, fromRect, host.startDriver()});
    host.cancel(begun.cancel);
    return begun;
  }

  // Same springs as the compositor's clip and radius: a running one keeps its velocity.
  template <typename Host> void startShape(Host& host, const Shape& now, const Shape& to, const SpringConfig& config, bool continueFrom) {
    for (std::size_t i = 0; i < to.size(); ++i) {
      m_shape[i] = continueFrom ? Spring(m_shape[i].value(), to[i], config, m_shape[i].velocity()) : Spring(now[i], to[i], config);
    }
    m_startMs = host.nowMs();
    m_requestedMs = m_startMs;
    m_shapeSec = 0.0;
  }

  // The rect did not change: no compositor animation, so no `started`.
  void startFadeNow() { m_fadeStartMs = m_startMs; }

  // The compositor got a new clip target; returns the generation its `started` must carry.
  template <typename Host> std::uint32_t retarget(Host& host, const SpringConfig& clip) {
    if (m_fade.settled()) {
      // A retarget outside a face change (content width): the faces stay shown while the shape moves.
      m_fade = Spring(1.0, 1.0, clip);
    }
    const Driver driver = m_morph.settled() ? host.startDriver() : m_morph.driver();
    m_morph.apply(typename Morph::Retarget{driver});
    return m_morph.gen();
  }

  template <typename Host> void started(Host& host, std::uint32_t gen, std::uint32_t timeMs, bool continueFrom) {
    if (!m_morph.apply(typename Morph::Started{gen}).accepted) {
      return;
    }
    // A retargeted compositor spring already steps one frame on its first tick.
    m_startMs = continueFrom ? timeMs - m_frameMs : timeMs;
    m_shapeSec = 0.0;
    m_fadeStartMs = timeMs;
    m_fadeSec = 0.0;
    host.redraw();
  }

  // The clip jumped: no morph runs any more. False when none ran.
  template <typename Host> bool jump(Host& host) {
    const auto r = m_morph.apply(typename Morph::Jump{});
    host.cancel(r.cancel);
    return r.wasMoving;
  }

  template <typename Host> void step(Host& host) {
    if (m_morph.settled()) {
      return;
    }
    const std::uint32_t now = host.nowMs();
    const std::uint32_t interval = now - m_lastStepMs;
    if (m_lastStepMs != 0 && interval > 0 && interval <= 50) {
      m_frameMs = interval;
    }
    m_lastStepMs = now;
    if (m_morph.phase() == Morph::Phase::AwaitStart) {
      if (static_cast<std::int32_t>(now - m_requestedMs) <= kStartWaitMs) {
        host.redraw();
        return;
      }
      m_morph.apply(typename Morph::StartTimeout{});
      m_startMs = now;
      m_shapeSec = 0.0;
      m_fadeStartMs = now;
      m_fadeSec = 0.0;
    }
    const double shapeSec = secondsSince(now, m_startMs);
    bool done = true;
    for (auto& spring : m_shape) {
      spring.step(shapeSec - m_shapeSec);
      done = done && spring.settled();
    }
    m_shapeSec = shapeSec;
    const double fadeSec = secondsSince(now, m_fadeStartMs);
    m_fade.step(fadeSec - m_fadeSec);
    m_fadeSec = fadeSec;
    if (done && m_fade.settled()) {
      cancelLater(host, m_morph.apply(typename Morph::Settle{}).cancel);
      host.settle();
      return;
    }
    host.reveal(static_cast<float>(m_fade.value()));
  }

  template <typename Host> void driverDone(Host& host, Driver driver) {
    if (m_morph.apply(typename Morph::DriverDone{driver}).accepted) {
      host.settle();
    }
  }

  template <typename Host> void reap(Host& host) {
    for (const Driver driver : m_ended) {
      host.cancel(driver);
    }
    m_ended.clear();
  }

private:
  // Wrap-safe seconds from startMs to nowMs, never negative.
  static double secondsSince(std::uint32_t nowMs, std::uint32_t startMs) {
    const auto ms = static_cast<std::int32_t>(nowMs - startMs);
    return (ms > 0 ? ms : 0) / 1000.0;
  }

  // A driver ended from inside its own tick: the host cannot cancel it there.
  template <typename Host> void cancelLater(Host& host, Driver driver) {
    if (driver == Driver{}) {
      return;
    }
    m_ended.push_back(driver);
    if (m_ended.size() == 1) {
      host.reapSoon();
    }
  }

  Morph m_morph;
  std::array<Spring, 5> m_shape;
  Spring m_fade; // 0..1 from the last face change
  std::uint32_t m_lastStepMs = 0;
  std::uint32_t m_frameMs = 16; // last measured step interval
  std::uint32_t m_startMs = 0;
  std::uint32_t m_fadeStartMs = 0;
  std::uint32_t m_requestedMs = 0;
  double m_shapeSec = 0.0;
  double m_fadeSec = 0.0;
  std::vector<Driver> m_ended;
};
