#pragma once

#include <cstdint>
#include <deque>

namespace andromeda {

  // Three-finger travel, in the accelerated motion libinput reports for gestures, for one full step: switching one
  // workspace, and scrolling the strip by one viewport. The overview reuses both so a swipe covers the same distance
  // whether the previews are up or not.
  inline constexpr double kSwipeWorkspacePx = 300.0;
  inline constexpr double kSwipeViewportPx = 1200.0;

  // Sliding-window swipe tracker (rulebook 7.7). Accumulates deltas since the gesture start, fits the velocity to the
  // last 100 ms of positions by least squares, and projects where the movement would coast to a stop under
  // exponential deceleration.
  class SwipeTracker {
  public:
    // Pushes a new reading into the tracker.
    void push(double delta, uint32_t timeMsec);
    void reset();

    [[nodiscard]] double pos() const { return m_pos; }
    [[nodiscard]] double velocity() const;
    [[nodiscard]] double projectedEndPos() const;

  private:
    struct Event {
      double pos = 0; // position after this reading
      uint32_t timeMsec = 0;
    };

    static constexpr int32_t kHistoryLimitMsec = 100;
    // Stepped pagers use 0.997, not rulebook 7.7's 0.998, so a moderate swipe advances one step.
    static constexpr double kDeceleration = 0.997; // per millisecond

    void trimHistory(uint32_t nowMsec);

    std::deque<Event> m_history;
    double m_pos = 0;
  };

} // namespace andromeda
