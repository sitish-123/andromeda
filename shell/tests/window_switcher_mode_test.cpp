#include "shell/switcher/window_switcher_mode.h"
#include "tests/test_check.h"

#include <cstdint>
#include <random>

namespace {

  using enum WindowSwitcherMode;

  // Drives the transitions the way WindowSwitcher does: an open from Hidden makes the surface, a commit or cancel
  // starts the Return, a reopen retargets it, its settle or a lost surface destroys the surface.
  struct Model {
    WindowSwitcherMode mode = Hidden;
    int surfaces = 0;
    bool returnRunning = false;

    bool set(WindowSwitcherMode next) {
      if (!windowSwitcherModeAllowed(mode, next)) {
        return false;
      }
      mode = next;
      return true;
    }
    void open() {
      const bool fresh = mode == Hidden;
      if (set(Pointer)) {
        surfaces += fresh ? 1 : 0;
        returnRunning = false;
      }
    }
    void shortcut() { set(Shortcut); }
    void close() {
      if (set(Closing)) {
        returnRunning = true;
      }
    }
    void settle() {
      if (!returnRunning) {
        return;
      }
      returnRunning = false;
      if (mode == Closing) {
        hide();
      }
    }
    void hide() {
      set(Hidden);
      surfaces = 0;
      returnRunning = false;
    }

    void checkInvariants() const {
      TEST_CHECK(surfaces == (mode == Hidden ? 0 : 1));
      TEST_CHECK((mode == Closing) == returnRunning);
    }
  };

  void testRandomSequencesKeepOneSurfaceAndNeverStickInClosing() {
    std::mt19937 rng(0x5317C4);
    for (int run = 0; run < 2000; ++run) {
      Model model;
      for (int step = 0; step < 40; ++step) {
        switch (std::uniform_int_distribution<int>(0, 6)(rng)) {
        case 0:
          model.open();
          break;
        case 1:
          model.shortcut();
          break;
        case 2:
          model.shortcut(); // cycle: the mode stays where it is
          break;
        case 3:
        case 4:
          model.close(); // commit or cancel
          break;
        case 5:
          model.settle();
          break;
        default:
          model.hide(); // surface lost
          break;
        }
        model.checkInvariants();
      }
      model.settle();
      TEST_CHECK(model.mode != Closing);
      model.checkInvariants();
    }
  }

  void testReopenDuringClosingReturnsToOpen() {
    TEST_CHECK(windowSwitcherModeAllowed(Closing, Pointer));
    TEST_CHECK(!windowSwitcherModeAllowed(Closing, Shortcut));
    TEST_CHECK(!windowSwitcherModeAllowed(Closing, Closing));
    TEST_CHECK(!windowSwitcherModeAllowed(Hidden, Closing));
    TEST_CHECK(!windowSwitcherModeAllowed(Pointer, Pointer));
  }

} // namespace

int main() {
  testRandomSequencesKeepOneSurfaceAndNeverStickInClosing();
  testReopenDuringClosingReturnsToOpen();
  return 0;
}
