#include "shell/session/session_end_phase.h"
#include "tests/test_check.h"

#include <cstdint>
#include <random>
#include <string>
#include <variant>

namespace {

  // The power overlay's modes as the portal sees them: Query covers Closing, Countdown and Blockers.
  enum class Mode : std::uint8_t { Choices, Query, Ending };

  session_end::PortalState portal(Mode mode, const session_end::Machine& machine) {
    return session_end::portalState(mode == Mode::Query, machine);
  }

} // namespace

// Random overlay and runner event sequences against a model: the portal shows QueryEnd while a query is open,
// Ending while the latest run has not failed, and Running only with neither; a failure of any other run changes
// nothing.
int main() {
  using session_end::PortalState;
  const std::string actions[] = {"logout", "reboot", "shutdown"};
  for (std::uint32_t seed = 1; seed <= 200; ++seed) {
    std::mt19937 rng(seed);
    session_end::Machine machine;
    Mode mode = Mode::Choices;
    std::uint64_t runs = 0;
    std::uint64_t latest = 0;
    bool latestFailed = false;
    std::string latestAction;
    for (int step = 0; step < 300; ++step) {
      const auto before = machine.phase();
      const PortalState portalBefore = portal(mode, machine);
      // Any run so far, the latest one, or one that never ran.
      const std::uint64_t generation = std::uniform_int_distribution<std::uint64_t>(0, runs + 1)(rng);
      switch (std::uniform_int_distribution<int>(0, 8)(rng)) {
      case 0: // begin: the overlay starts a session end from its choices
        if (mode == Mode::Choices) {
          mode = Mode::Query;
        }
        break;
      case 1: // countdown or blockers: still a query
        break;
      case 2: // cancel the query
        if (mode == Mode::Query) {
          mode = Mode::Choices;
        }
        break;
      case 3: // hide: run() hides the sheet after finish, or the user closes it
        TEST_CHECK(machine.apply(session_end::Hide{}));
        mode = Mode::Choices;
        TEST_CHECK(machine.phase() == before);
        break;
      case 4: // finish: the query ends and the action runs
        if (mode == Mode::Query) {
          mode = Mode::Ending;
          latestAction = actions[std::uniform_int_distribution<int>(0, 2)(rng)];
          TEST_CHECK(machine.apply(session_end::Run{.action = latestAction}));
          latest = ++runs;
          latestFailed = false;
        }
        break;
      case 5:
        TEST_CHECK(machine.apply(session_end::Show{}));
        TEST_CHECK(machine.phase() == before);
        break;
      case 6:
        (void)machine.apply(session_end::Succeeded{.generation = generation});
        TEST_CHECK(machine.phase() == before);
        break;
      default: {
        const bool accepted = machine.apply(session_end::Failed{.generation = generation});
        if (generation == latest && latest != 0 && !latestFailed) {
          TEST_CHECK(accepted);
          latestFailed = true;
        } else {
          TEST_CHECK(!accepted);
          TEST_CHECK(machine.phase() == before);
          TEST_CHECK(portal(mode, machine) == portalBefore);
        }
        break;
      }
      }
      const bool inFlight = latest != 0 && !latestFailed;
      TEST_CHECK(machine.ending() == inFlight);
      if (inFlight) {
        const session_end::Phase expected = session_end::Ending{.action = latestAction, .generation = latest};
        TEST_CHECK(machine.phase() == expected);
      } else {
        TEST_CHECK(machine.generation() == 0);
      }
      const PortalState now = portal(mode, machine);
      TEST_CHECK((mode == Mode::Query) == (now == PortalState::QueryEnd));
      TEST_CHECK((now == PortalState::Running) == (mode != Mode::Query && !inFlight));
      TEST_CHECK((now == PortalState::Ending) == (mode != Mode::Query && inFlight));
    }
  }
  return 0;
}
