#include "system/memory_pressure_poll_source.h"

#include "core/log.h"

#include <cerrno>
#include <cstring>
#include <fstream>
#include <string>
#include <utility>

#include <fcntl.h>
#include <unistd.h>

namespace {
  constexpr Logger kLog("memory");
  // 150 ms of "some" stall in a 2 s window; unprivileged triggers need a window that is a multiple of 2 s.
  constexpr char kTrigger[] = "some 150000 2000000";
  constexpr auto kMinInterval = std::chrono::minutes(1);
} // namespace

MemoryPressurePollSource::MemoryPressurePollSource(std::function<void()> onPressure)
    : m_onPressure(std::move(onPressure)) {
  std::ifstream cgroup("/proc/self/cgroup");
  std::string line;
  if (!std::getline(cgroup, line) || !line.starts_with("0::")) {
    return;
  }
  const std::string path = "/sys/fs/cgroup" + line.substr(3) + "/memory.pressure";
  m_fd = open(path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
  if (m_fd >= 0 && write(m_fd, kTrigger, sizeof(kTrigger)) < 0) {
    close(m_fd);
    m_fd = -1;
  }
  if (m_fd < 0) {
    kLog.info("no memory pressure trigger on {}: {}", path, std::strerror(errno));
  }
}

MemoryPressurePollSource::~MemoryPressurePollSource() {
  if (m_fd >= 0) {
    close(m_fd);
  }
}

void MemoryPressurePollSource::doAddPollFds(std::vector<pollfd>& fds) {
  if (m_fd >= 0) {
    fds.push_back({.fd = m_fd, .events = POLLPRI, .revents = 0});
  }
}

void MemoryPressurePollSource::dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) {
  if (m_fd < 0 || startIdx >= fds.size()) {
    return;
  }
  const short revents = fds[startIdx].revents;
  if ((revents & POLLERR) != 0) {
    // The cgroup went away: no more events can come.
    close(m_fd);
    m_fd = -1;
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  if ((revents & POLLPRI) != 0 && now - m_last >= kMinInterval) {
    m_last = now;
    kLog.info("memory pressure: dropping caches");
    m_onPressure();
  }
}
