#pragma once

#include "app/poll_source.h"
#include "core/process/async_process_manager.h"

class ProcessPollSource : public PollSource {
public:
  explicit ProcessPollSource(process::AsyncProcessManager& manager) : m_manager(manager) {}

  [[nodiscard]] int pollTimeoutMs() const override { return m_manager.pollTimeoutMs(); }

  void dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) override { m_manager.dispatch(fds, startIdx); }

protected:
  void doAddPollFds(std::vector<pollfd>& fds) override { m_manager.addPollFds(fds); }

private:
  process::AsyncProcessManager& m_manager;
};
