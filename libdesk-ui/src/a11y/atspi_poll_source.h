#pragma once

#include "a11y/atspi_bridge.h"
#include "core/poll_source.h"

#include <algorithm>
#include <chrono>

// Dispatches the accessibility bus and coalesces tree updates; adds no fd and no timeout while no AT is active.
class AtspiPollSource final : public PollSource {
public:
  [[nodiscard]] int pollTimeoutMs() const override {
    auto& bridge = AtspiBridge::instance();
    if (!bridge.active()) {
      return -1;
    }
    const auto pollData = bridge.pollData();
    if (!pollData.has_value()) {
      return 0;
    }
    int timeout = pollData->getPollTimeout();
    if (bridge.dirty()) {
      const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - m_lastUpdate).count();
      const int untilUpdate = std::max(0, AtspiBridge::kUpdateIntervalMs - static_cast<int>(waited));
      timeout = timeout < 0 ? untilUpdate : std::min(timeout, untilUpdate);
    }
    return timeout;
  }

  void dispatch(const std::vector<pollfd>& /*fds*/, std::size_t /*startIdx*/) override {
    auto& bridge = AtspiBridge::instance();
    bridge.dispatch();
    if (bridge.dirty() && Clock::now() - m_lastUpdate >= std::chrono::milliseconds(AtspiBridge::kUpdateIntervalMs)) {
      bridge.update();
      m_lastUpdate = Clock::now();
    }
  }

protected:
  void doAddPollFds(std::vector<pollfd>& fds) override {
    if (const auto pollData = AtspiBridge::instance().pollData(); pollData.has_value()) {
      fds.push_back({.fd = pollData->fd, .events = pollData->events, .revents = 0});
      fds.push_back({.fd = pollData->eventFd, .events = POLLIN, .revents = 0});
    }
  }

private:
  using Clock = std::chrono::steady_clock;
  Clock::time_point m_lastUpdate{};
};
