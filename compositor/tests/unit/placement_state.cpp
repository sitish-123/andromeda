#include "check.h"
#include "view/placement_state.h"

#include <random>

using namespace andromeda;

namespace {

  constexpr PlacementEvent kEvents[] = {
      PlacementEvent::Map,           PlacementEvent::Float,           PlacementEvent::Tile,
      PlacementEvent::RememberFullscreen, PlacementEvent::LayoutFloat, PlacementEvent::ClearLayoutFloat,
      PlacementEvent::Pin,           PlacementEvent::PinTile,         PlacementEvent::Unpin,
      PlacementEvent::EnterFullscreen, PlacementEvent::ExitFullscreen, PlacementEvent::DropEdgeRestore,
      PlacementEvent::RestorePin,    PlacementEvent::FloatMaximize,   PlacementEvent::FloatUnmaximize,
      PlacementEvent::EdgeMaximize,  PlacementEvent::EdgeUnmaximize,  PlacementEvent::DropMaximize,
      PlacementEvent::Snap,          PlacementEvent::Unsnap,          PlacementEvent::Attach,
      PlacementEvent::Unmap,
  };

  // Invariants every reachable placement holds, whatever the event order.
  void checkShape(const Placement& placement) {
    const bool pinned = placement.arrangement == Arrangement::Pinned;
    if (placement.arrangement == Arrangement::Tiled) {
      CHECK(placement.fit == Fit::Free || placement.fit == Fit::EdgeMaximized);
      CHECK(!restoresPin(placement.restoreTo));
      CHECK(!placement.snap);
    }
    CHECK(placement.unpinTo != Arrangement::Pinned);
    if (!pinned && !restoresPin(placement.restoreTo)) {
      CHECK(placement.unpinTo == Arrangement::Floating);
    }
    if (pinned) {
      CHECK(!restoresPin(placement.restoreTo));
    }
  }

} // namespace

ANDROMEDA_TEST(randomEventsKeepThePlacementShape) {
  std::mt19937 rng(7);
  std::uniform_int_distribution<size_t> pick(0, std::size(kEvents) - 1);
  for (int run = 0; run < 200; ++run) {
    Placement placement;
    for (int step = 0; step < 200; ++step) {
      const PlacementInput input{
          .event = kEvents[pick(rng)], .flag = (rng() & 1U) != 0, .spot = SnapRegion::TopLeft
      };
      const Placement before = placement;
      if (!applyPlacementEvent(placement, input)) {
        CHECK(placement == before);
      }
      checkShape(placement);
      if (input.event == PlacementEvent::Attach) {
        CHECK(placement == Placement{});
      }
      if (input.event == PlacementEvent::Unmap) {
        CHECK(placement.fit == Fit::Free);
        CHECK(placement.restoreTo == RestoreTo::None);
      }
    }
  }
}

int main() { return RUN_TESTS(); }
