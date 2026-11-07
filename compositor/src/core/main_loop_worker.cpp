#include "core/main_loop_worker.h"

#include <sys/eventfd.h>
#include <unistd.h>
#include <wayland-server-core.h>

#include <utility>

namespace andromeda {

  MainLoopWorker::MainLoopWorker(wl_event_loop* loop) {
    m_eventFd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (m_eventFd >= 0 && loop != nullptr) {
      m_source = wl_event_loop_add_fd(loop, m_eventFd, WL_EVENT_READABLE, onReady, this);
    }
    m_thread = std::jthread([this](const std::stop_token& stop) { run(stop); });
  }

  void MainLoopWorker::stop() {
    m_thread.request_stop();
    m_wake.notify_all();
    if (m_thread.joinable()) {
      m_thread.join();
    }
  }

  MainLoopWorker::~MainLoopWorker() {
    stop();
    if (m_source != nullptr) {
      wl_event_source_remove(m_source);
    }
    if (m_eventFd >= 0) {
      ::close(m_eventFd);
    }
  }

  void MainLoopWorker::postJob(Job job) {
    if (m_source == nullptr) {
      return;
    }
    {
      const std::scoped_lock lock(m_mutex);
      m_jobs.push_back(std::move(job));
    }
    m_wake.notify_one();
  }

  void MainLoopWorker::run(const std::stop_token& stop) {
    std::unique_lock lock(m_mutex);
    while (m_wake.wait(lock, stop, [this] { return !m_jobs.empty(); })) {
      Job job = std::move(m_jobs.front());
      m_jobs.pop_front();
      lock.unlock();
      std::function<void()> result = job();
      lock.lock();
      m_results.push_back(std::move(result));
      const std::uint64_t one = 1;
      [[maybe_unused]] const ssize_t written = ::write(m_eventFd, &one, sizeof(one));
    }
  }

  int MainLoopWorker::onReady(int fd, std::uint32_t /*mask*/, void* data) {
    auto* self = static_cast<MainLoopWorker*>(data);
    std::uint64_t count = 0;
    [[maybe_unused]] const ssize_t got = ::read(fd, &count, sizeof(count));
    std::deque<std::function<void()>> results;
    {
      const std::scoped_lock lock(self->m_mutex);
      results.swap(self->m_results);
    }
    for (auto& result : results) {
      result();
    }
    return 0;
  }

} // namespace andromeda
