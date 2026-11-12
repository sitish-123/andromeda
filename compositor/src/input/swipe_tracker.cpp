#include "input/swipe_tracker.h"

namespace andromeda {

  void SwipeTracker::push(double delta, uint32_t timeMsec) {
    // For the events we care about, timestamps should always increase monotonically.
    if (!m_history.empty()) {
      const uint32_t last = m_history.back().timeMsec;
      if (static_cast<int32_t>(timeMsec - last) < 0) {
        return;
      }
    }

    m_pos += delta;
    m_history.push_back({m_pos, timeMsec});

    trimHistory(timeMsec);
  }

  void SwipeTracker::reset() {
    m_history.clear();
    m_pos = 0;
  }

  double SwipeTracker::velocity() const {
    if (m_history.size() < 2) {
      return 0.0;
    }
    // Least-squares slope of position over time, in px per ms.
    const uint32_t origin = m_history.front().timeMsec;
    double meanT = 0.0;
    double meanP = 0.0;
    for (const Event& event : m_history) {
      meanT += static_cast<double>(static_cast<int32_t>(event.timeMsec - origin));
      meanP += event.pos;
    }
    const auto count = static_cast<double>(m_history.size());
    meanT /= count;
    meanP /= count;
    double covariance = 0.0;
    double variance = 0.0;
    for (const Event& event : m_history) {
      const double t = static_cast<double>(static_cast<int32_t>(event.timeMsec - origin)) - meanT;
      covariance += t * (event.pos - meanP);
      variance += t * t;
    }
    // Seconds, matching a px/s velocity.
    return variance > 0.0 ? covariance / variance * 1000.0 : 0.0;
  }

  double SwipeTracker::projectedEndPos() const {
    return m_pos + velocity() * kDeceleration / (1.0 - kDeceleration) / 1000.0;
  }

  void SwipeTracker::trimHistory(uint32_t nowMsec) {
    while (!m_history.empty()) {
      const Event& first = m_history.front();
      if (static_cast<int32_t>(nowMsec - first.timeMsec) <= kHistoryLimitMsec) {
        break;
      }
      m_history.pop_front();
    }
  }

} // namespace andromeda
