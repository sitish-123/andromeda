#include "view/deferred_unfullscreen.h"

#include "check.h"

using andromeda::DeferredUnfullscreen;
using andromeda::FullscreenRequestDisposition;

namespace {
  using namespace std::chrono_literals;
  constexpr DeferredUnfullscreen::TimePoint kStart{};
} // namespace

ANDROMEDA_TEST(inactiveUnfullscreenRequestIsParked) {
  DeferredUnfullscreen state;
  CHECK(state.observeClientRequest(false, false, true, kStart) == FullscreenRequestDisposition::Park);
  CHECK(state.pending());
}

ANDROMEDA_TEST(activationConsumesAFreshParkedRequest) {
  DeferredUnfullscreen state;
  static_cast<void>(state.observeClientRequest(false, false, true, kStart));
  CHECK(state.takeOnActivation(kStart + DeferredUnfullscreen::kGrace));
  CHECK(!state.pending());
  CHECK(!state.takeOnActivation(kStart + DeferredUnfullscreen::kGrace));
}

ANDROMEDA_TEST(expiredParkedRequestIsDiscarded) {
  DeferredUnfullscreen state;
  static_cast<void>(state.observeClientRequest(false, false, true, kStart));
  CHECK(!state.takeOnActivation(kStart + DeferredUnfullscreen::kGrace + 1ms));
  CHECK(!state.pending());
}

ANDROMEDA_TEST(laterFullscreenRequestSupersedesParkedUnfullscreen) {
  DeferredUnfullscreen state;
  static_cast<void>(state.observeClientRequest(false, false, true, kStart));
  CHECK(state.observeClientRequest(true, false, true, kStart + 1ms) == FullscreenRequestDisposition::Acknowledge);
  CHECK(!state.pending());
  CHECK(!state.takeOnActivation(kStart + 2ms));
}

ANDROMEDA_TEST(applyingAnotherRequestClearsParkedUnfullscreen) {
  DeferredUnfullscreen state;
  static_cast<void>(state.observeClientRequest(false, false, true, kStart));
  CHECK(state.observeClientRequest(true, true, false, kStart + 1ms) == FullscreenRequestDisposition::Apply);
  CHECK(!state.pending());
}

int main() { return RUN_TESTS(); }
