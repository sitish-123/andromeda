#pragma once

#include "core/poll_source.h"

#include <chrono>
#include <functional>

// Runs onPressure when the shell's own cgroup stalls on memory (a PSI trigger on its memory.pressure, docs/qos.md §5),
// at most once a minute.
class MemoryPressurePollSource final : public PollSource {
public:
  explicit MemoryPressurePollSource(std::function<void()> onPressure);
  ~MemoryPressurePollSource() override;
  MemoryPressurePollSource(const MemoryPressurePollSource&) = delete;
  MemoryPressurePollSource& operator=(const MemoryPressurePollSource&) = delete;

  void dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) override;

protected:
  void doAddPollFds(std::vector<pollfd>& fds) override;

private:
  int m_fd = -1;
  std::function<void()> m_onPressure;
  std::chrono::steady_clock::time_point m_last{};
};
