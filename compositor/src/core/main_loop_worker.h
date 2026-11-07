#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <thread>

struct wl_event_loop;
struct wl_event_source;

namespace andromeda {

  // One thread for slow reads (/proc scans) that must stay off the compositor main thread. `done` runs on the event
  // loop. Jobs run in order; jobs and results still pending when the worker is destroyed are dropped.
  class MainLoopWorker {
  public:
    explicit MainLoopWorker(wl_event_loop* loop);
    ~MainLoopWorker();

    // Finishes the running job and ends the thread; queued jobs and results are dropped. For an owner that writes
    // its final state in place and must not race a job on the same file.
    void stop();

    MainLoopWorker(const MainLoopWorker&) = delete;
    MainLoopWorker& operator=(const MainLoopWorker&) = delete;

    // `work` runs on the worker thread and must touch no main-thread state; `done` gets its result on the loop.
    template <typename Result> void post(std::function<Result()> work, std::function<void(Result)> done) {
      postJob([work = std::move(work), done = std::move(done)]() mutable -> std::function<void()> {
        auto result = std::make_shared<Result>(work());
        return [done = std::move(done), result] { done(std::move(*result)); };
      });
    }

  private:
    using Job = std::function<std::function<void()>()>;
    void postJob(Job job);
    void run(const std::stop_token& stop);
    static int onReady(int fd, std::uint32_t mask, void* data);

    int m_eventFd = -1;
    wl_event_source* m_source = nullptr;
    std::mutex m_mutex;
    std::condition_variable_any m_wake;
    std::deque<Job> m_jobs;
    std::deque<std::function<void()>> m_results;
    // Last member: joined before the queues it reads go away.
    std::jthread m_thread;
  };

} // namespace andromeda
