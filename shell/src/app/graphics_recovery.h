#pragma once

#include "core/deferred_call.h"
#include "core/timer_manager.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <utility>

// When to rebuild the GPU context after a reset: one rebuild at a time, backing off between failed attempts, and
// giving up when a reset outlives the retries (a dead GPU must not spin the loop).
class GraphicsRecovery {
public:
  static constexpr int kMaxAttempts = 8;
  static constexpr std::chrono::milliseconds kBaseDelay{250};
  static constexpr std::chrono::milliseconds kMaxDelay{5000};

  explicit GraphicsRecovery(std::function<void()> rebuild) : m_rebuild(std::move(rebuild)) {}

  // A reset while a rebuild waits joins it; the rebuild runs outside the render callback.
  void onReset() {
    if (m_state == State::Pending) {
      return;
    }
    m_state = State::Pending;
    DeferredCall::callLater([this] { run(); });
  }

  // Called by the rebuild when it threw; schedules the next attempt. Returns the delay, or nothing when it gave up.
  std::optional<std::chrono::milliseconds> failed() {
    ++m_failedAttempts;
    if (m_failedAttempts >= kMaxAttempts) {
      return std::nullopt;
    }
    const auto delay = std::min(kBaseDelay * (1 << (m_failedAttempts - 1)), kMaxDelay);
    m_state = State::Pending;
    m_timer.start(delay, [this] { run(); });
    return delay;
  }

  void succeeded() noexcept { m_failedAttempts = 0; }
  [[nodiscard]] int failedAttempts() const noexcept { return m_failedAttempts; }

private:
  enum class State : std::uint8_t { Idle, Pending };

  void run() {
    m_state = State::Idle;
    m_rebuild();
  }

  std::function<void()> m_rebuild;
  State m_state = State::Idle;
  int m_failedAttempts = 0;
  Timer m_timer;
};
