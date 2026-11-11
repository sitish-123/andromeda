#include "shell/panel/panel_lifecycle.h"
#include "tests/test_check.h"

#include <cstdint>
#include <random>
#include <variant>

namespace {

  using namespace panel_lifecycle;
  using Machine = Lifecycle<int>;

  bool sameReplay(const Machine::Replay& a, const Machine::Replay& b) {
    if (a.index() != b.index()) {
      return false;
    }
    if (const auto* open = std::get_if<int>(&a)) {
      return *open == std::get<int>(b);
    }
    if (const auto* close = std::get_if<CloseRequest>(&a)) {
      return close->animate == std::get<CloseRequest>(b).animate;
    }
    return true;
  }

  void latestRequestWinsAfterInitializing() {
    Machine machine;
    TEST_CHECK(machine.apply(BeginInit{}).accepted);
    TEST_CHECK(machine.apply(Requested<int>{1}).accepted);
    TEST_CHECK(machine.apply(Requested<int>{CloseRequest{false}}).accepted);
    TEST_CHECK(machine.apply(Requested<int>{2}).accepted);
    const Machine::Result ended = machine.apply(InitEnded{true});
    TEST_CHECK(ended.accepted && machine.phase() == Phase::Open);
    TEST_CHECK(sameReplay(ended.replay, Machine::Replay{2}));
  }

  void destroyWhileInitializingReplaysAsClose() {
    Machine machine;
    TEST_CHECK(machine.apply(BeginInit{}).accepted);
    TEST_CHECK(!machine.apply(Destroyed{}).accepted);
    TEST_CHECK(machine.phase() == Phase::Initializing);
    const Machine::Result ended = machine.apply(InitEnded{true});
    TEST_CHECK(ended.accepted && machine.phase() == Phase::Open);
    TEST_CHECK(sameReplay(ended.replay, Machine::Replay{CloseRequest{false}}));
  }

  void randomSequencesKeepTheModel() {
    for (std::uint32_t seed = 1; seed <= 5000; ++seed) {
      std::mt19937 rng(seed);
      Machine machine;
      Phase phase = Phase::Closed;
      Machine::Replay latest;
      int nextOpen = 0;
      for (int i = 0; i < 64; ++i) {
        const auto pick = rng() % 6;
        Machine::Result result;
        bool expectAccepted = false;
        Phase expectPhase = phase;
        Machine::Replay expectReplay;
        switch (pick) {
        case 0:
          result = machine.apply(BeginInit{});
          expectAccepted = phase == Phase::Closed;
          if (expectAccepted) {
            expectPhase = Phase::Initializing;
            latest = {};
          }
          break;
        case 1:
        case 2: {
          const bool ok = pick == 1;
          result = machine.apply(InitEnded{ok});
          expectAccepted = phase == Phase::Initializing;
          if (expectAccepted) {
            expectPhase = ok ? Phase::Open : Phase::Closed;
            expectReplay = latest;
            latest = {};
          }
          break;
        }
        case 3:
          result = machine.apply(BeginClose{});
          expectAccepted = phase == Phase::Open;
          expectPhase = expectAccepted ? Phase::Closing : phase;
          break;
        case 4:
          result = machine.apply(Destroyed{});
          expectAccepted = phase != Phase::Initializing;
          expectPhase = expectAccepted ? Phase::Closed : phase;
          if (!expectAccepted) {
            latest = CloseRequest{false};
          }
          break;
        default: {
          Requested<int> request{++nextOpen};
          if ((rng() & 1U) != 0) {
            request.request = CloseRequest{(rng() & 1U) != 0};
          }
          const auto asReplay = std::visit([](auto r) { return Machine::Replay{r}; }, request.request);
          result = machine.apply(request);
          expectAccepted = phase == Phase::Initializing;
          if (expectAccepted) {
            latest = asReplay;
          }
          break;
        }
        }
        if (result.accepted != expectAccepted || machine.phase() != expectPhase
            || !sameReplay(result.replay, expectReplay)) {
          TEST_CHECK(false && "panel lifecycle diverged from the model");
          return;
        }
        phase = expectPhase;
        TEST_CHECK(machine.initializing() == (phase == Phase::Initializing));
        TEST_CHECK(machine.closing() == (phase == Phase::Closing));
      }
    }
  }

} // namespace

int main() {
  latestRequestWinsAfterInitializing();
  destroyWhileInitializingReplaysAsClose();
  randomSequencesKeepTheModel();
  return 0;
}
