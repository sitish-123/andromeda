#pragma once

#include <cstdint>
#include <string>
#include <variant>

// The session end the power overlay runs. Ending holds from the run of a logout, reboot or shutdown until that run
// fails; a success ends the session in Ending. Only the run that set Ending can leave it, so a late failure of an
// older or unrelated action never tells Inhibit portal monitors the session goes on.
namespace session_end {

  struct Idle {
    bool operator==(const Idle&) const = default;
  };
  struct Ending {
    std::string action;
    std::uint64_t generation = 0;
    bool operator==(const Ending&) const = default;
  };
  using Phase = std::variant<Idle, Ending>;

  struct Run {
    std::string action;
  };
  struct Succeeded {
    std::uint64_t generation = 0;
  };
  struct Failed {
    std::uint64_t generation = 0;
  };
  // The overlay surface going away or coming back never changes the phase.
  struct Hide {};
  struct Show {};
  using Event = std::variant<Run, Succeeded, Failed, Hide, Show>;

  class Machine {
  public:
    [[nodiscard]] const Phase& phase() const { return m_phase; }
    [[nodiscard]] bool ending() const { return std::holds_alternative<Ending>(m_phase); }
    // Generation of the run that holds Ending; 0 while Idle.
    [[nodiscard]] std::uint64_t generation() const {
      const auto* ending = std::get_if<Ending>(&m_phase);
      return ending != nullptr ? ending->generation : 0;
    }

    // The one transition. Returns false, with the phase unchanged, for an event that does not apply.
    bool apply(const Event& event) {
      if (const auto* run = std::get_if<Run>(&event)) {
        // A newer run supersedes one that hangs: it now owns Ending.
        m_phase = Ending{.action = run->action, .generation = ++m_lastGeneration};
        return true;
      }
      if (const auto* done = std::get_if<Succeeded>(&event)) {
        return ending() && generation() == done->generation;
      }
      if (const auto* failed = std::get_if<Failed>(&event)) {
        if (!ending() || generation() != failed->generation) {
          return false;
        }
        m_phase = Idle{};
        return true;
      }
      return true;
    }

  private:
    Phase m_phase = Idle{};
    std::uint64_t m_lastGeneration = 0;
  };

  // Inhibit portal session-state values.
  enum class PortalState : std::uint32_t { Running = 1, QueryEnd = 2, Ending = 3 };

  // The one source of the portal state. `queryOpen`: a session end asks apps now (windows closing, blockers or the
  // countdown); it wins over an earlier run, whose late failure then changes nothing.
  [[nodiscard]] inline PortalState portalState(bool queryOpen, const Machine& machine) {
    if (queryOpen) {
      return PortalState::QueryEnd;
    }
    return machine.ending() ? PortalState::Ending : PortalState::Running;
  }

} // namespace session_end
