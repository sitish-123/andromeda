#pragma once

#include "core/process/process.h"

#include <chrono>
#include <memory>
#include <mutex>
#include <poll.h>
#include <vector>

namespace process {

  // Runs every callback-based process::runAsync call without a helper thread: each child gets a
  // pidfd (pidfd_open(2)) the shell's main loop polls alongside its stdout/stderr pipes, woken on
  // exit or output instead of a thread blocked in waitpid/read. One instance for the process.
  //
  // start() can be called from any thread (e.g. TemplateApplyService's worker), while
  // pollTimeoutMs()/addPollFds()/dispatch() are driven by the main loop's own thread through
  // ProcessPollSource; m_mutex makes that safe. pumpOnce() lets a caller with no main loop of its
  // own (HookRunner's blocking waits, tests) make progress by spending its own thread's time
  // instead of the main loop's.
  class AsyncProcessManager {
  public:
    static AsyncProcessManager& instance();

    AsyncProcessManager(const AsyncProcessManager&) = delete;
    AsyncProcessManager& operator=(const AsyncProcessManager&) = delete;

    // Forks and execs args, wiring callbacks to fire once observed to exit (or time out, or
    // options.cancel turns true). False on any spawn failure, matching runAsync.
    [[nodiscard]] bool start(std::vector<std::string> args, RunCallbacks callbacks, RunOptions options);

    [[nodiscard]] int pollTimeoutMs() const;
    void addPollFds(std::vector<pollfd>& fds);
    void dispatch(const std::vector<pollfd>& fds, std::size_t startIdx);

    // One poll()+dispatch() pass, blocking the calling thread up to timeout for fd activity.
    void pumpOnce(std::chrono::milliseconds timeout);

  private:
    AsyncProcessManager() = default;
    ~AsyncProcessManager();

    struct Child;
    mutable std::mutex m_mutex;
    std::vector<std::unique_ptr<Child>> m_children;
  };

} // namespace process
