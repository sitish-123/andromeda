#include "check.h"
#include "view/configure_state.h"

#include <random>

using namespace andromeda;

namespace {

  // The wlroots side of one xdg toplevel: serials, the configure idle, the suspended state and sizes.
  struct Client {
    bool mapped = false;
    bool destroyed = false;
    bool queued = false;
    unsigned scheduledSerial = 0;
    unsigned sentSerial = 0;
    unsigned currentSerial = 0;
    bool suspended = false;
    int scheduledWidth = 0;
    int sentWidth = 0;
    int currentWidth = 0;

    [[nodiscard]] bool sizeLanded() const { return scheduledWidth == 0 || scheduledWidth == currentWidth; }
    [[nodiscard]] bool outstanding() const { return queued || currentSerial != scheduledSerial; }
    // The formula Server::settled() polled before the state existed.
    [[nodiscard]] bool settled() const {
      return destroyed || !mapped || !outstanding() || (suspended && sizeLanded());
    }
    [[nodiscard]] ConfigureFacts facts() const {
      return {.queued = queued, .acked = currentSerial == scheduledSerial, .excused = suspended && sizeLanded()};
    }
  };

  unsigned g_serial = 0;

  void schedule(Client& client) {
    if (!client.queued) {
      client.scheduledSerial = ++g_serial;
      client.queued = true;
    }
  }

} // namespace

ANDROMEDA_TEST(aHiddenWindowThatOwesOnlyTheSuspendConfigureSettlesWithoutACommit) {
  ConfigureState state = ConfigureState::Unmapped;
  int edges = 0;
  const auto edge = [&edges] { ++edges; };
  CHECK(applyConfigureEvent(state, ConfigureEvent::Map, {}, edge));
  CHECK(applyConfigureEvent(state, ConfigureEvent::Scheduled, {.queued = true, .acked = false}, edge));
  CHECK_EQ(state, ConfigureState::Queued);
  CHECK(applyConfigureEvent(state, ConfigureEvent::Sent, {.acked = false}, edge));
  CHECK_EQ(state, ConfigureState::AwaitCommit);
  CHECK(applyConfigureEvent(state, ConfigureEvent::Scheduled, {.queued = true, .acked = false, .excused = true}, edge));
  CHECK_EQ(state, ConfigureState::Suspended);
  CHECK_EQ(edges, 1);
}

ANDROMEDA_TEST(aConfigureAnotherModuleScheduledIsTrackedFromItsSend) {
  ConfigureState state = ConfigureState::Unmapped;
  int edges = 0;
  const auto edge = [&edges] { ++edges; };
  CHECK(applyConfigureEvent(state, ConfigureEvent::Map, {}, edge));
  CHECK(applyConfigureEvent(state, ConfigureEvent::Sent, {.acked = false}, edge));
  CHECK_EQ(state, ConfigureState::AwaitCommit);
  CHECK(applyConfigureEvent(state, ConfigureEvent::Commit, {}, edge));
  CHECK_EQ(state, ConfigureState::Idle);
  CHECK_EQ(edges, 1);
}

// Schedules, sends, acked and plain commits, suspend changes, maps, unmaps and destroys in any order.
ANDROMEDA_TEST(randomEventsMatchThePolledFormulaAndEveryEntryIntoSettledFiresOnce) {
  for (unsigned seed = 1; seed <= 300; ++seed) {
    std::mt19937 rng(seed);
    Client client;
    ConfigureState state = ConfigureState::Unmapped;
    int edges = 0;
    const auto edge = [&edges] { ++edges; };
    for (int step = 0; step < 500; ++step) {
      const bool wasSettled = client.settled();
      const ConfigureState before = state;
      const int edgesBefore = edges;
      std::optional<ConfigureEvent> event;
      bool valid = !client.destroyed;
      switch (rng() % 10) {
        case 0:
          schedule(client);
          event = ConfigureEvent::Scheduled;
          break;
        case 1:
          client.scheduledWidth = static_cast<int>(rng() % 3) * 100;
          schedule(client);
          event = ConfigureEvent::Scheduled;
          break;
        case 2:
          client.suspended = !client.suspended;
          schedule(client);
          event = ConfigureEvent::Scheduled;
          break;
        case 3:
          client.scheduledWidth = client.currentWidth;
          event = ConfigureEvent::Scheduled;
          break;
        case 4:
          if (client.queued) {
            client.queued = false;
            client.sentSerial = client.scheduledSerial;
            client.sentWidth = client.scheduledWidth;
            event = ConfigureEvent::Sent;
          }
          break;
        case 5:
          if (client.sentSerial != 0) {
            client.currentSerial = client.sentSerial;
            client.currentWidth = client.sentWidth;
            event = ConfigureEvent::Commit;
          }
          break;
        case 6:
          if (rng() % 2 == 0) {
            client.currentWidth = static_cast<int>(rng() % 3) * 100;
          }
          event = ConfigureEvent::Commit;
          break;
        case 7:
          valid = valid && !client.mapped;
          if (valid) {
            client.mapped = true;
            client.scheduledSerial = client.currentSerial = client.sentSerial = ++g_serial;
            client.queued = false;
          }
          event = ConfigureEvent::Map;
          break;
        case 8:
          valid = valid && client.mapped;
          if (valid) {
            client.mapped = false;
            client.queued = false;
          }
          event = ConfigureEvent::Unmap;
          break;
        default:
          if (rng() % 8 == 0) {
            if (client.destroyed) {
              client = Client{};
              state = ConfigureState::Unmapped;
              continue;
            }
            client.mapped = false;
            client.destroyed = true;
            event = ConfigureEvent::Destroy;
          }
          break;
      }
      if (!event) {
        continue;
      }
      const bool accepted = applyConfigureEvent(state, *event, client.facts(), edge);
      CHECK_EQ(accepted, valid);
      if (!accepted) {
        CHECK_EQ(state, before);
      }
      CHECK_EQ(configureSettled(state), client.settled());
      CHECK_EQ(configureOwed(state), !client.destroyed && client.mapped && client.outstanding());
      CHECK_EQ(edges - edgesBefore, !wasSettled && client.settled() ? 1 : 0);
    }
  }
}

int main() { return RUN_TESTS(); }
