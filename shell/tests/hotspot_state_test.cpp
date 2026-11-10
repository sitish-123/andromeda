#include "dbus/network/hotspot_state.h"
#include "tests/test_check.h"

#include <optional>
#include <random>
#include <vector>

namespace {

  enum class Call : std::uint8_t { Activate, Deactivate };

  enum class Step : std::uint8_t {
    RequestOn,
    RequestOff,
    ReplyOk,
    ReplyFailed,
    StartQuery,
    FinishQuery,
    ExternalToggle,
    LateFailure,
    NmActivated,
  };

  struct Query {
    std::uint64_t token;
    HotspotObservation seen;
  };

  bool pending(HotspotState state) { return state == HotspotState::Activating || state == HotspotState::Deactivating; }

  // The state a query result must leave when it counts.
  HotspotState expected(HotspotState prior, HotspotObservation seen) {
    if (prior == HotspotState::Activating) {
      return seen == HotspotObservation::On ? HotspotState::On
             : seen == HotspotObservation::Off ? HotspotState::Off
                                               : HotspotState::Activating;
    }
    return seen == HotspotObservation::Off ? HotspotState::Off : HotspotState::On;
  }

  // Random requests, replies, overlapping queries that finish out of order, late failures and external toggles,
  // against a model of NetworkManager and of the one D-Bus call in flight.
  void runSequence(std::uint32_t seed) {
    std::mt19937 random(seed);
    std::uniform_int_distribution<int> pick(0, 8);
    HotspotMachine machine;
    HotspotObservation world = HotspotObservation::Off;
    std::optional<Call> inFlight;
    bool starting = false;
    std::optional<bool> lastTarget;
    std::vector<Query> queries;
    std::uint64_t lastIssued = 0;
    std::uint64_t fence = 0;

    const auto issueQuery = [&] {
      lastIssued = machine.beginQuery();
      queries.push_back({lastIssued, world});
    };
    const auto finishQuery = [&](std::size_t index) {
      const Query query = queries[index];
      queries.erase(queries.begin() + static_cast<std::ptrdiff_t>(index));
      const HotspotState prior = machine.state();
      const std::string priorError = machine.error();
      machine.observe(query.token, query.seen, "connection gone");
      if (query.token != lastIssued || query.token <= fence || inFlight.has_value()) {
        TEST_CHECK(machine.state() == prior && machine.error() == priorError);
        return;
      }
      TEST_CHECK(machine.state() == expected(prior, query.seen));
      if (starting && machine.state() == HotspotState::Off) {
        TEST_CHECK(!machine.error().empty());
      } else if (machine.state() != prior) {
        TEST_CHECK(machine.error().empty());
      } else {
        TEST_CHECK(machine.error() == priorError);
      }
      if (machine.state() != HotspotState::Activating) {
        starting = false;
      }
    };
    const auto reply = [&](bool ok) {
      const bool accepted = machine.apply(
          inFlight == Call::Activate ? (ok ? HotspotEvent::ActivateOk : HotspotEvent::ActivateFailed)
                                     : (ok ? HotspotEvent::DeactivateOk : HotspotEvent::DeactivateFailed),
          "denied");
      TEST_CHECK(accepted);
      fence = lastIssued;
      if (ok) {
        world = inFlight == Call::Activate ? HotspotObservation::Activating : HotspotObservation::Off;
        starting = inFlight == Call::Activate;
        TEST_CHECK(machine.error().empty());
      } else {
        TEST_CHECK(!machine.error().empty());
      }
      inFlight.reset();
      issueQuery();
    };

    for (int step = 0; step < 300; ++step) {
      const auto action = static_cast<Step>(pick(random));
      const HotspotState prior = machine.state();
      switch (action) {
      case Step::RequestOn:
      case Step::RequestOff: {
        const bool on = action == Step::RequestOn;
        const bool accepted = machine.apply(on ? HotspotEvent::RequestOn : HotspotEvent::RequestOff);
        TEST_CHECK(accepted == (prior == (on ? HotspotState::Off : HotspotState::On)));
        if (accepted) {
          TEST_CHECK(machine.error().empty());
          inFlight = on ? Call::Activate : Call::Deactivate;
          lastTarget = on;
          fence = lastIssued;
        }
        break;
      }
      case Step::ReplyOk:
      case Step::ReplyFailed:
        if (inFlight.has_value()) {
          reply(action == Step::ReplyOk);
        } else {
          TEST_CHECK(!machine.apply(HotspotEvent::ActivateOk) && !machine.apply(HotspotEvent::DeactivateOk));
          TEST_CHECK(!machine.apply(HotspotEvent::ActivateFailed) && !machine.apply(HotspotEvent::DeactivateFailed));
        }
        break;
      case Step::StartQuery:
        issueQuery();
        break;
      case Step::FinishQuery:
        if (!queries.empty()) {
          finishQuery(random() % queries.size());
        }
        break;
      case Step::ExternalToggle:
        world = world == HotspotObservation::Off ? HotspotObservation::On : HotspotObservation::Off;
        issueQuery();
        break;
      case Step::LateFailure:
        if (world == HotspotObservation::Activating) {
          world = HotspotObservation::Off;
          issueQuery();
        }
        break;
      case Step::NmActivated:
        if (world == HotspotObservation::Activating) {
          world = HotspotObservation::On;
          issueQuery();
        }
        break;
      }
      TEST_CHECK(pending(machine.state()) == (inFlight.has_value() || starting));
      TEST_CHECK(!lastTarget.has_value() || machine.requestedOn() == *lastTarget);
    }

    if (inFlight.has_value()) {
      reply(true);
    }
    // A new query overlaps every one in flight; they finish in random order, the new one last.
    issueQuery();
    while (queries.size() > 1) {
      finishQuery(random() % (queries.size() - 1));
    }
    const HotspotState prior = machine.state();
    finishQuery(0);
    TEST_CHECK(machine.state() == expected(prior, world));
  }

} // namespace

int main() {
  for (std::uint32_t seed = 1; seed <= 500; ++seed) {
    runSequence(seed);
  }
  return 0;
}
