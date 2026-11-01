#pragma once

#include <chrono>
#include <cstddef>
#include <thread>
#include <vector>

// A bounded set of worker threads that start with queued work and exit after sitting idle for kIdleExit. Every
// member except joinAll() runs with the owner's queue mutex held.
class IdleWorkerSlots {
public:
  static constexpr auto kIdleExit = std::chrono::seconds(30);

  explicit IdleWorkerSlots(std::size_t count) : m_threads(count), m_alive(count, false) {}

  // Starts loop(slot) in a free slot, if any; call after queueing work. A slot freed by exited() has already
  // released the mutex, so joining it here is immediate.
  template <typename Loop> void spawn(Loop loop) {
    for (std::size_t slot = 0; slot < m_threads.size(); ++slot) {
      if (!m_alive[slot]) {
        if (m_threads[slot].joinable()) {
          m_threads[slot].join();
        }
        m_threads[slot] = std::thread(loop, slot);
        m_alive[slot] = true;
        return;
      }
    }
  }

  // Called by the worker in slot just before it returns.
  void exited(std::size_t slot) { m_alive[slot] = false; }

  // Without the mutex, after shutdown is signalled.
  void joinAll() {
    for (auto& thread : m_threads) {
      if (thread.joinable()) {
        thread.join();
      }
    }
  }

private:
  std::vector<std::thread> m_threads;
  std::vector<bool> m_alive;
};
