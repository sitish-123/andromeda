#include "dbus/network/hotspot_state.h"
#include "tests/test_check.h"

#include <optional>
#include <random>
#include <vector>

namespace {

  enum class Call : std::uint8_t { Activate, Deactivate, Create, Remove };

  enum class Restart : std::uint8_t { None, Deactivating, Rearming };

  enum class Step : std::uint8_t {
    RequestOn,
    RequestOnBeside,
    RequestOff,
    ReplyOk,
    ReplyFailed,
    StartQuery,
    FinishQuery,
    ExternalToggle,
    LateFailure,
    NmActivated,
    DeviceAppears,
    TimerFires,
    PlanArrives,
    // The Wi-Fi client roams to another channel.
    Roam,
    // A hotspot on `<client>ap` that an earlier shell or System1 run started comes up in the world.
    Leftover,
  };

  struct Query {
    std::uint64_t token;
    HotspotObservation seen;
  };

  bool pending(HotspotState state) { return state == HotspotState::Activating || state == HotspotState::Deactivating; }

  // The state a query result must leave when it counts. `owned`: the hotspot runs on an interface the machine made,
  // so a hotspot that is gone first removes it.
  HotspotState expected(HotspotState prior, HotspotObservation seen, bool owned) {
    const HotspotState gone = owned ? HotspotState::Deactivating : HotspotState::Off;
    if (prior == HotspotState::Activating) {
      return seen == HotspotObservation::On || seen == HotspotObservation::OnBesideClient ? HotspotState::On
             : seen == HotspotObservation::Off ? gone
                                               : HotspotState::Activating;
    }
    if (prior == HotspotState::On) {
      return seen == HotspotObservation::Off ? gone : HotspotState::On;
    }
    return seen == HotspotObservation::Off ? HotspotState::Off : HotspotState::On;
  }

  // Random requests, replies, overlapping queries that finish out of order, late failures, external toggles,
  // access-point interfaces that fail, never appear in NetworkManager (timer) or appear, roams that restart the
  // hotspot, leftover hotspots that the machine adopts, and requests before the plan is known, against a model of
  // NetworkManager, System1 and the one call in flight.
  void runSequence(std::uint32_t seed) {
    std::mt19937 random(seed);
    std::uniform_int_distribution<int> pick(0, 14);
    HotspotMachine machine;
    HotspotObservation world = HotspotObservation::Off;
    std::optional<Call> inFlight;
    bool starting = false;
    bool awaitingDevice = false;
    bool planKnown = false;
    // Deactivating: a roam's Deactivate is in flight; its success re-arms the interface. Rearming: that
    // CreateApInterface call is in flight; its success starts the activation on the new channel.
    Restart restart = Restart::None;
    // An interface exists whose removal the machine has not started yet.
    bool owned = false;
    int created = 0;
    int removed = 0;
    std::optional<bool> lastTarget;
    std::vector<Query> queries;
    std::uint64_t lastIssued = 0;
    std::uint64_t fence = 0;

    // Starts the work the machine asked for, the way NmHotspot does.
    const auto drain = [&] {
      const std::optional<HotspotWork> work = machine.takeWork();
      TEST_CHECK(!machine.takeWork().has_value());
      if (!work.has_value()) {
        return;
      }
      TEST_CHECK(!inFlight.has_value() && !awaitingDevice);
      switch (*work) {
      case HotspotWork::Activate:
        TEST_CHECK(!owned);
        inFlight = Call::Activate;
        break;
      case HotspotWork::CreateInterface:
        TEST_CHECK(owned == (restart == Restart::Rearming));
        inFlight = Call::Create;
        break;
      case HotspotWork::AwaitDevice:
        TEST_CHECK(owned);
        awaitingDevice = true;
        break;
      case HotspotWork::ActivateOnInterface:
        TEST_CHECK(owned);
        inFlight = Call::Activate;
        break;
      case HotspotWork::Deactivate:
        inFlight = Call::Deactivate;
        break;
      case HotspotWork::RemoveInterface:
        TEST_CHECK(owned);
        owned = false;
        ++removed;
        inFlight = Call::Remove;
        break;
      }
    };
    const auto issueQuery = [&] {
      lastIssued = machine.beginQuery();
      queries.push_back({lastIssued, world});
    };
    const auto finishQuery = [&](std::size_t index) {
      const Query query = queries[index];
      queries.erase(queries.begin() + static_cast<std::ptrdiff_t>(index));
      const HotspotState prior = machine.state();
      const std::string priorError = machine.error();
      const bool priorOwned = owned;
      machine.observe(query.token, query.seen, "connection gone");
      if (query.token != lastIssued || query.token <= fence || inFlight.has_value() || awaitingDevice) {
        TEST_CHECK(machine.state() == prior && machine.error() == priorError);
        TEST_CHECK(!machine.takeWork().has_value());
        return;
      }
      drain();
      TEST_CHECK(machine.state() == expected(prior, query.seen, priorOwned));
      if (!priorOwned && query.seen == HotspotObservation::OnBesideClient && prior != HotspotState::Activating) {
        owned = true;
        ++created;
      }
      if (starting && query.seen == HotspotObservation::Off) {
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
      const Call call = *inFlight;
      const std::string priorError = machine.error();
      HotspotEvent event{};
      switch (call) {
      case Call::Activate:
        event = ok ? HotspotEvent::ActivateOk : HotspotEvent::ActivateFailed;
        break;
      case Call::Deactivate:
        event = ok ? HotspotEvent::DeactivateOk : HotspotEvent::DeactivateFailed;
        break;
      case Call::Create:
        event = ok ? HotspotEvent::InterfaceCreated : HotspotEvent::InterfaceFailed;
        break;
      case Call::Remove:
        event = ok ? HotspotEvent::InterfaceRemoved : HotspotEvent::RemoveFailed;
        break;
      }
      TEST_CHECK(machine.apply(event, "denied"));
      fence = lastIssued;
      inFlight.reset();
      const Restart wasRestart = std::exchange(restart, Restart::None);
      if (call == Call::Create && ok && wasRestart != Restart::Rearming) {
        owned = true;
        ++created;
      }
      if (ok && call == Call::Activate) {
        world = HotspotObservation::Activating;
        starting = true;
      } else if (ok && call == Call::Deactivate) {
        world = HotspotObservation::Off;
      }
      const bool removing = owned
                            && ((call == Call::Activate && !ok)
                                || (call == Call::Deactivate && ok != (wasRestart == Restart::Deactivating))
                                || (call == Call::Create && !ok && wasRestart == Restart::Rearming));
      if (wasRestart == Restart::Deactivating && ok) {
        restart = Restart::Rearming;
      }
      drain();
      if (wasRestart == Restart::Deactivating && ok) {
        TEST_CHECK(inFlight == Call::Create && owned);
      } else if (wasRestart == Restart::Rearming && ok) {
        TEST_CHECK(inFlight == Call::Activate);
      }
      if (call == Call::Remove) {
        TEST_CHECK(machine.state() == HotspotState::Off);
        TEST_CHECK(ok ? machine.error() == priorError : !machine.error().empty());
      } else if (ok) {
        TEST_CHECK(machine.error().empty());
      } else {
        TEST_CHECK(!machine.error().empty());
      }
      if (removing) {
        TEST_CHECK(inFlight == Call::Remove);
      }
      if (call != Call::Create) {
        issueQuery();
      }
    };

    for (int step = 0; step < 300; ++step) {
      const auto action = static_cast<Step>(pick(random));
      const HotspotState prior = machine.state();
      switch (action) {
      case Step::RequestOn:
      case Step::RequestOnBeside:
      case Step::RequestOff: {
        const bool on = action != Step::RequestOff;
        const HotspotEvent event = action == Step::RequestOn        ? HotspotEvent::RequestOn
                                   : action == Step::RequestOnBeside ? HotspotEvent::RequestOnBesideClient
                                                                     : HotspotEvent::RequestOff;
        const bool accepted = machine.apply(event);
        TEST_CHECK(accepted == (prior == (on ? HotspotState::Off : HotspotState::On) && (!on || planKnown)));
        if (accepted) {
          TEST_CHECK(machine.error().empty());
          lastTarget = on;
          fence = lastIssued;
          drain();
          TEST_CHECK(inFlight
                     == (action == Step::RequestOn         ? Call::Activate
                         : action == Step::RequestOnBeside ? Call::Create
                                                           : Call::Deactivate));
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
          TEST_CHECK(!machine.apply(HotspotEvent::InterfaceCreated) && !machine.apply(HotspotEvent::InterfaceFailed));
          TEST_CHECK(!machine.apply(HotspotEvent::InterfaceRemoved) && !machine.apply(HotspotEvent::RemoveFailed));
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
      case Step::PlanArrives:
        TEST_CHECK(machine.apply(HotspotEvent::PlanKnown) == !planKnown);
        TEST_CHECK(machine.planKnown());
        planKnown = true;
        break;
      case Step::Roam: {
        const bool accepted = machine.apply(HotspotEvent::ClientChannelChanged);
        TEST_CHECK(accepted == (prior == HotspotState::On && owned));
        if (accepted) {
          restart = Restart::Deactivating;
          fence = lastIssued;
          drain();
          TEST_CHECK(inFlight == Call::Deactivate && machine.state() == HotspotState::Activating);
        }
        break;
      }
      case Step::Leftover:
        if (prior == HotspotState::Off && world == HotspotObservation::Off && !inFlight.has_value()) {
          world = HotspotObservation::OnBesideClient;
          issueQuery();
        }
        break;
      case Step::DeviceAppears:
      case Step::TimerFires: {
        const bool timeout = action == Step::TimerFires;
        const bool accepted =
            machine.apply(timeout ? HotspotEvent::DeviceTimeout : HotspotEvent::DeviceReady, "no device");
        TEST_CHECK(accepted == awaitingDevice);
        if (accepted) {
          awaitingDevice = false;
          fence = lastIssued;
          drain();
          TEST_CHECK(inFlight == (timeout ? Call::Remove : Call::Activate));
          TEST_CHECK(timeout != machine.error().empty());
        }
        break;
      }
      }
      TEST_CHECK(!machine.takeWork().has_value());
      TEST_CHECK(pending(machine.state()) == (inFlight.has_value() || starting || awaitingDevice));
      TEST_CHECK(!lastTarget.has_value() || machine.requestedOn() == *lastTarget);
      TEST_CHECK(removed <= created);
      TEST_CHECK(machine.state() != HotspotState::Off || (removed == created && !owned));
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
    const bool priorOwned = owned;
    const bool counts = !inFlight.has_value() && !awaitingDevice;
    finishQuery(0);
    TEST_CHECK(!counts || machine.state() == expected(prior, world, priorOwned));

    // Turning everything off removes every interface the machine created.
    for (int round = 0; round < 20; ++round) {
      if (awaitingDevice) {
        TEST_CHECK(machine.apply(HotspotEvent::DeviceTimeout, "no device"));
        awaitingDevice = false;
        fence = lastIssued;
        drain();
      } else if (inFlight.has_value()) {
        reply(true);
      } else if (starting) {
        world = HotspotObservation::On;
        issueQuery();
        finishQuery(queries.size() - 1);
      } else if (machine.state() == HotspotState::On) {
        TEST_CHECK(machine.apply(HotspotEvent::RequestOff));
        fence = lastIssued;
        drain();
      } else {
        break;
      }
    }
    TEST_CHECK(machine.state() == HotspotState::Off && removed == created && !owned);
  }

} // namespace

int main() {
  const auto channelIs = [](std::uint32_t mhz, std::string_view band, std::uint32_t channel) {
    const auto got = hotspotChannelOf(mhz);
    return got.has_value() && got->band == band && got->channel == channel;
  };
  TEST_CHECK(channelIs(2412, "bg", 1));
  TEST_CHECK(channelIs(2437, "bg", 6));
  TEST_CHECK(channelIs(2484, "bg", 14));
  TEST_CHECK(channelIs(5180, "a", 36));
  TEST_CHECK(channelIs(5745, "a", 149));
  TEST_CHECK(!hotspotChannelOf(5955).has_value());
  TEST_CHECK(!hotspotChannelOf(0).has_value());

  for (std::uint32_t seed = 1; seed <= 500; ++seed) {
    runSequence(seed);
  }
  return 0;
}
