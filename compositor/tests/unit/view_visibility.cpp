#include "check.h"
#include "view/view_visibility.h"

#include <random>

using namespace andromeda;

ANDROMEDA_TEST(aDeferredTiledOpenerStaysHiddenWhenItsWorkspaceComesBack) {
  WindowVisibility window;
  window.lifecycle = *lifecycleAfter(window.lifecycle, LifecycleEvent::Map);
  window.lifecycle = *lifecycleAfter(window.lifecycle, LifecycleEvent::DeferOpening);
  window.placeShows = false;
  window.placeShows = true;
  CHECK(!window.shown());
  window.lifecycle = *lifecycleAfter(window.lifecycle, LifecycleEvent::ResumeOpening);
  CHECK(window.shown());
}

// Map, unmap, deferral and its arrange, tab and minimize changes, and workspace switches in any order.
ANDROMEDA_TEST(randomEventsShowAWindowOnlyWhenMappedPlacedAndNotHidden) {
  for (unsigned seed = 1; seed <= 300; ++seed) {
    std::mt19937 rng(seed);
    WindowVisibility window;
    bool mapped = false;
    bool deferred = false;
    for (int step = 0; step < 500; ++step) {
      const unsigned event = rng() % 7;
      if (event < 4) {
        const auto lifecycleEvent = static_cast<LifecycleEvent>(event);
        const std::optional<Lifecycle> next = lifecycleAfter(window.lifecycle, lifecycleEvent);
        bool accepted = false;
        switch (lifecycleEvent) {
          case LifecycleEvent::Map:
            accepted = !mapped;
            mapped = true;
            break;
          case LifecycleEvent::DeferOpening:
            accepted = mapped && !deferred;
            deferred = deferred || accepted;
            break;
          case LifecycleEvent::ResumeOpening:
            accepted = deferred;
            deferred = false;
            break;
          case LifecycleEvent::Unmap:
            accepted = mapped;
            mapped = false;
            deferred = false;
            break;
        }
        CHECK_EQ(next.has_value(), accepted);
        if (next) {
          window.lifecycle = *next;
        }
      } else if (event == 4) {
        window.tabHidden = !window.tabHidden;
      } else if (event == 5) {
        window.minimized = !window.minimized;
      } else {
        window.placeShows = !window.placeShows;
      }
      CHECK_EQ(window.lifecycle != Lifecycle::Unmapped, mapped);
      CHECK_EQ(window.lifecycle == Lifecycle::OpeningDeferred, deferred);
      CHECK_EQ(window.shown(), mapped && !deferred && !window.tabHidden && !window.minimized && window.placeShows);
    }
  }
}

int main() { return RUN_TESTS(); }
