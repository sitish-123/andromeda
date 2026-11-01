#pragma once

#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

// The one background thread for CPU work (docs/m0-foundation.md §5). It starts on the first post and exits after
// kIdleExit with an empty queue. Work must not touch its owner; it returns data, and the completion runs on the main
// loop only while the owner's alive token lives.
class SharedWorker {
public:
  static constexpr auto kIdleExit = std::chrono::seconds(5);

  static SharedWorker& instance();

  explicit SharedWorker(std::chrono::milliseconds idleExit = kIdleExit) : m_idleExit(idleExit) {}
  SharedWorker(const SharedWorker&) = delete;
  SharedWorker& operator=(const SharedWorker&) = delete;
  ~SharedWorker();

  void post(std::function<void()> work);

  template <typename Result>
  void post(std::weak_ptr<void> alive, std::function<Result()> work, std::function<void(Result)> done) {
    post([alive = std::move(alive), work = std::move(work), done = std::move(done)]() mutable {
      auto result = std::make_shared<Result>(work());
      runOnMainLoop([alive = std::move(alive), done = std::move(done), result] {
        if (!alive.expired()) {
          done(std::move(*result));
        }
      });
    });
  }

  // True while the worker thread runs; for the thread-count check.
  [[nodiscard]] bool running();

private:
  static void runOnMainLoop(std::function<void()> fn);
  void loop();

  std::mutex m_mutex;
  std::condition_variable m_wake;
  std::deque<std::function<void()>> m_queue;
  std::thread m_thread;
  bool m_running = false;
  bool m_stopping = false;
  std::chrono::milliseconds m_idleExit;
};
