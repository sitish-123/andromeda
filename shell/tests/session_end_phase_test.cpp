#include "shell/session/session_end_phase.h"
#include "tests/test_check.h"

#include <cstdint>
#include <random>
#include <string>
#include <variant>

// Random event sequences through the session-end machine against a model: Ending holds exactly while the latest run
// has not failed, and a failure of any other run never changes the phase.
int main() {
  const std::string actions[] = {"logout", "reboot", "shutdown"};
  for (std::uint32_t seed = 1; seed <= 200; ++seed) {
    std::mt19937 rng(seed);
    session_end::Machine machine;
    std::uint64_t runs = 0;
    std::uint64_t latest = 0;
    bool latestFailed = false;
    std::string latestAction;
    for (int step = 0; step < 300; ++step) {
      const auto before = machine.phase();
      // Any run so far, the latest one, or one that never ran.
      const std::uint64_t generation = std::uniform_int_distribution<std::uint64_t>(0, runs + 1)(rng);
      bool accepted = false;
      switch (std::uniform_int_distribution<int>(0, 4)(rng)) {
      case 0:
        latestAction = actions[std::uniform_int_distribution<int>(0, 2)(rng)];
        accepted = machine.apply(session_end::Run{.action = latestAction});
        latest = ++runs;
        latestFailed = false;
        TEST_CHECK(accepted);
        break;
      case 1:
        accepted = machine.apply(session_end::Succeeded{.generation = generation});
        TEST_CHECK(machine.phase() == before);
        break;
      case 2:
        accepted = machine.apply(session_end::Failed{.generation = generation});
        if (generation == latest && latest != 0 && !latestFailed) {
          TEST_CHECK(accepted);
          latestFailed = true;
        } else {
          TEST_CHECK(!accepted);
          TEST_CHECK(machine.phase() == before);
        }
        break;
      case 3:
        accepted = machine.apply(session_end::Hide{});
        TEST_CHECK(machine.phase() == before);
        break;
      default:
        accepted = machine.apply(session_end::Show{});
        TEST_CHECK(machine.phase() == before);
        break;
      }
      const bool inFlight = latest != 0 && !latestFailed;
      TEST_CHECK(machine.ending() == inFlight);
      if (inFlight) {
        const session_end::Phase expected = session_end::Ending{.action = latestAction, .generation = latest};
        TEST_CHECK(machine.phase() == expected);
      } else {
        TEST_CHECK(machine.generation() == 0);
      }
    }
  }
  return 0;
}
