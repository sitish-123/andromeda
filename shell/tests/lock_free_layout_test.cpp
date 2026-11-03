#include "shell/lockscreen/lock_free_layout.h"
#include "tests/test_check.h"

int main() {
  constexpr float kW = 1280.0F;
  constexpr float kH = 800.0F;
  const AuthPlacement centered = placeAuth({.sw = kW, .sh = kH, .scale = 1});
  const lock_free::Zones z = lock_free::zones(centered, kW, kH, 1);

  // A drop over the account column is refused; the same box well above it lands on the 32 grid.
  {
    const AuthRect overAuth{centered.columnCx - 60.0F, centered.capsule.y - 20.0F, 120.0F, 64.0F};
    TEST_CHECK(!lock_free::place(overAuth, z, false, lock_free::Zone::None, true, kW, kH, 1).has_value());
    const auto dropped = lock_free::place({101.0F, 77.0F, 120.0F, 64.0F}, z, false, lock_free::Zone::None, true, kW, kH, 1);
    TEST_CHECK(dropped.has_value() && dropped->x == 96.0F && dropped->y == 64.0F);
  }

  // Snap to zones: a box dragged out of the Top band stays inside it.
  {
    const AuthRect box{200.0F, z.top.bottom() + 100.0F, 120.0F, 56.0F};
    const auto kept = lock_free::place(box, z, true, lock_free::Zone::Top, true, kW, kH, 1);
    TEST_CHECK(kept.has_value() && kept->bottom() <= z.top.bottom() && kept->y >= z.top.y);
  }

  // Arrow keys move 4 (Shift 32) and are not snapped back to the 32 grid; Ctrl+arrows resize, never below 56.
  {
    const AuthRect box{96.0F, 64.0F, 120.0F, 64.0F};
    const AuthRect moved = lock_free::nudge(box, 1, 0, false, false, 1);
    const auto kept = lock_free::place(moved, z, false, lock_free::Zone::None, false, kW, kH, 1);
    TEST_CHECK(kept.has_value() && kept->x == 100.0F);
    TEST_CHECK(lock_free::nudge(box, 0, 1, true, false, 1).y == 96.0F);
    TEST_CHECK(lock_free::nudge({0, 0, 60.0F, 60.0F}, -1, -1, true, true, 1).w == 56.0F);
  }
  return 0;
}
