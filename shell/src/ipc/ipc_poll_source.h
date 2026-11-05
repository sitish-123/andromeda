#pragma once

#include <algorithm>
#include <vector>

#include "core/poll_source.h"
#include "ipc/ipc_service.h"

class IpcPollSource final : public PollSource {
public:
  explicit IpcPollSource(IpcService& ipc) : m_ipc(ipc) {}

  void dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) override {
    // Copied first: a handler that pumps the loop inside dispatch() adds poll fds again.
    const std::size_t end = std::min(startIdx + m_added, fds.size());
    std::vector<int> closed;
    for (std::size_t i = startIdx; i < end; ++i) {
      if (fds[i].revents == 0) {
        continue;
      }
      if (fds[i].fd == m_ipc.listenFd()) {
        m_ipc.dispatch();
      } else {
        closed.push_back(fds[i].fd);
      }
    }
    for (const int fd : closed) {
      m_ipc.subscriberClosed(fd);
    }
  }

protected:
  void doAddPollFds(std::vector<pollfd>& fds) override {
    const std::size_t before = fds.size();
    if (m_ipc.listenFd() >= 0) {
      fds.push_back({.fd = m_ipc.listenFd(), .events = POLLIN, .revents = 0});
    }
    // No events asked: POLLHUP or POLLERR (always reported) means the peer is gone. A client half-closes its write
    // side after the request, so POLLIN would fire at once.
    for (const int fd : m_ipc.subscriberFds()) {
      fds.push_back({.fd = fd, .events = 0, .revents = 0});
    }
    m_added = fds.size() - before;
  }

private:
  IpcService& m_ipc;
  std::size_t m_added = 0;
};
