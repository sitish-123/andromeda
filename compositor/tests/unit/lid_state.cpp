#include "input/lid_state.h"

#include "check.h"

using umbriel::LidState;
using umbriel::LidStateCoordinator;

namespace {

  void checkTransition(std::optional<LidState> transition, LidState expected) {
    CHECK(transition.has_value());
    if (transition) {
      CHECK(*transition == expected);
    }
  }

} // namespace

UMBRIEL_TEST(initialClosedWaitsForReadinessAndSuppressesOpen) {
  LidStateCoordinator coordinator;
  const auto source = coordinator.addSource(LidState::Open);
  coordinator.updateSource(source, LidState::Closed);
  CHECK(!coordinator.takeTransition().has_value());

  coordinator.setReady();
  checkTransition(coordinator.takeTransition(), LidState::Closed);
  CHECK(!coordinator.takeTransition().has_value());
}

UMBRIEL_TEST(initialOpenPublishesAfterReadiness) {
  LidStateCoordinator coordinator;
  const auto source = coordinator.addSource(LidState::Open);
  (void)source;
  CHECK(!coordinator.takeTransition().has_value());

  coordinator.setReady();
  checkTransition(coordinator.takeTransition(), LidState::Open);
}

UMBRIEL_TEST(runtimeTransitionsPublishExactlyOnce) {
  LidStateCoordinator coordinator;
  const auto source = coordinator.addSource(LidState::Open);
  coordinator.setReady();
  checkTransition(coordinator.takeTransition(), LidState::Open);

  coordinator.updateSource(source, LidState::Closed);
  checkTransition(coordinator.takeTransition(), LidState::Closed);
  CHECK(!coordinator.takeTransition().has_value());
  coordinator.updateSource(source, LidState::Closed);
  CHECK(!coordinator.takeTransition().has_value());

  coordinator.updateSource(source, LidState::Open);
  checkTransition(coordinator.takeTransition(), LidState::Open);
  CHECK(!coordinator.takeTransition().has_value());
}

UMBRIEL_TEST(zeroSourceIntervalPreservesPublishedStateAcrossResume) {
  LidStateCoordinator coordinator;
  const auto first = coordinator.addSource(LidState::Closed);
  coordinator.setReady();
  checkTransition(coordinator.takeTransition(), LidState::Closed);

  coordinator.removeSource(first);
  CHECK(!coordinator.takeTransition().has_value());

  const auto stillClosed = coordinator.addSource(LidState::Open);
  coordinator.updateSource(stillClosed, LidState::Closed);
  CHECK(!coordinator.takeTransition().has_value());
  coordinator.removeSource(stillClosed);
  CHECK(!coordinator.takeTransition().has_value());

  const auto reopenedSource = coordinator.addSource(LidState::Open);
  (void)reopenedSource;
  checkTransition(coordinator.takeTransition(), LidState::Open);
}

UMBRIEL_TEST(multipleSourcesRemainClosedUntilEveryLidOpens) {
  LidStateCoordinator coordinator;
  const auto first = coordinator.addSource(LidState::Open);
  const auto second = coordinator.addSource(LidState::Open);
  coordinator.setReady();
  checkTransition(coordinator.takeTransition(), LidState::Open);

  coordinator.updateSource(first, LidState::Closed);
  checkTransition(coordinator.takeTransition(), LidState::Closed);
  coordinator.updateSource(second, LidState::Closed);
  coordinator.updateSource(first, LidState::Open);
  CHECK(!coordinator.takeTransition().has_value());

  coordinator.removeSource(second);
  checkTransition(coordinator.takeTransition(), LidState::Open);
}

UMBRIEL_TEST(preReadySourcesCoalesceToOneAggregateTransition) {
  LidStateCoordinator coordinator;
  const auto open = coordinator.addSource(LidState::Open);
  const auto closed = coordinator.addSource(LidState::Closed);
  (void)open;
  (void)closed;
  CHECK(!coordinator.takeTransition().has_value());

  coordinator.setReady();
  checkTransition(coordinator.takeTransition(), LidState::Closed);
  CHECK(!coordinator.takeTransition().has_value());
}

int main() { return RUN_TESTS(); }
