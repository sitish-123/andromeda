#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>

namespace noctalia::theme {

  // Runs template hooks concurrently with bounded parallelism. The runner owns no
  // threads: each hook is spawned through process::runAsync, which reports completion
  // through process::AsyncProcessManager (the shell's main loop, or a caller's own
  // thread via waitIdle()'s pump when no main loop is driving it). The shared state
  // stays alive past destruction so a hook's callback always has somewhere to land.
  class HookRunner {
  public:
    static constexpr std::size_t kDefaultMaxConcurrent = 4;

    explicit HookRunner(std::size_t maxConcurrent = kDefaultMaxConcurrent);
    ~HookRunner();

    HookRunner(const HookRunner&) = delete;
    HookRunner& operator=(const HookRunner&) = delete;

    // Starts a hook, or queues it while maxConcurrent hooks are already running.
    // Hooks whose generation predates the current one are discarded so superseded
    // requests cannot leave stale state.
    void enqueue(std::string command, std::uint64_t generation);
    // Discards queued hooks from older generations. Hooks that already started
    // keep running; waitIdle() waits them out.
    void invalidateBefore(std::uint64_t generation);
    void waitIdle();
    // Drops the queued backlog and releases waitIdle() callers. Running hooks keep
    // going; the destructor waits for them.
    void requestShutdown();
    [[nodiscard]] std::size_t pendingCount() const;

  private:
    struct QueuedHook {
      std::string command;
      std::uint64_t generation = 0;
    };

    struct State {
      std::mutex mutex;
      std::condition_variable idleCv;
      std::deque<QueuedHook> queue;
      std::size_t running = 0;
      std::size_t maxConcurrent = kDefaultMaxConcurrent;
      std::uint64_t currentGeneration = 0;
      bool shutdown = false;
    };

    static void pump(const std::shared_ptr<State>& state);
    [[nodiscard]] static bool launch(const std::shared_ptr<State>& state, const std::string& command);

    std::shared_ptr<State> m_state;
  };

} // namespace noctalia::theme
