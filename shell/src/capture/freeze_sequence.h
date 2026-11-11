#pragma once

#include <cstdint>
#include <utility>
#include <variant>
#include <vector>

// The freeze-screen capture: one output after another. Each run has a generation; a step queued by an older run
// is Stale and must not take an output from, or cancel the capture of, the run that replaced it.
namespace freeze_capture {

  struct Stale {};
  struct Finished {};
  template <typename Output> struct Capture {
    Output output;
  };

  template <typename Output> class Sequence {
  public:
    using Step = std::variant<Stale, Capture<Output>, Finished>;

    [[nodiscard]] bool capturing() const noexcept { return m_phase == Phase::Capturing; }
    [[nodiscard]] bool current(std::uint64_t generation) const noexcept {
      return capturing() && generation == m_generation;
    }

    std::uint64_t begin(std::vector<Output> outputs) {
      ++m_generation;
      m_pending = std::move(outputs);
      m_phase = Phase::Capturing;
      return m_generation;
    }

    Step next(std::uint64_t generation) {
      if (!current(generation)) {
        return Stale{};
      }
      if (m_pending.empty()) {
        m_phase = Phase::Idle;
        return Finished{};
      }
      Output output = std::move(m_pending.front());
      m_pending.erase(m_pending.begin());
      return Capture<Output>{std::move(output)};
    }

    void abort() {
      ++m_generation;
      m_pending.clear();
      m_phase = Phase::Idle;
    }

  private:
    enum class Phase : std::uint8_t { Idle, Capturing };

    Phase m_phase = Phase::Idle;
    std::uint64_t m_generation = 0;
    std::vector<Output> m_pending;
  };

} // namespace freeze_capture
