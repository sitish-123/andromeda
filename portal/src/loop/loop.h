#pragma once

#include <cstdint>
#include <functional>
#include <memory>

namespace xdpu {

  class Loop {
  public:
    Loop();
    ~Loop();

    Loop(const Loop&) = delete;
    Loop& operator=(const Loop&) = delete;
    Loop(Loop&&) = delete;
    Loop& operator=(Loop&&) = delete;

    using FdCallback = std::function<void(uint32_t events)>;
    using TimerCallback = std::function<void()>;

    int addFd(int fd, uint32_t events, FdCallback cb);
    void updateFd(int id, uint32_t events);
    void removeFd(int id);

    int addTimer(int timeoutMs, TimerCallback cb);
    void removeTimer(int id);

    void run();
    void quit();

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
  };

} // namespace xdpu
