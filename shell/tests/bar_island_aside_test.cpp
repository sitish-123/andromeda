#include "shell/bar/island_aside.h"
#include "tests/test_check.h"

#include <cmath>

namespace {

  bool near(float a, float b) { return std::abs(a - b) < 1e-4F; }

  bool rectNear(const bar_detail::IslandRect& a, const bar_detail::IslandRect& b) {
    return near(a[0], b[0]) && near(a[1], b[1]) && near(a[2], b[2]) && near(a[3], b[3]);
  }

} // namespace

// Side islands step aside for the overview: in place at 0, slid one thickness toward their bar end and shrunk to
// nothing at 1, along the bar's own axis.
int main() {
  using bar_detail::islandAsideRect;
  const bar_detail::IslandRect rest{100.0F, 4.0F, 200.0F, 32.0F};

  TEST_CHECK(rectNear(islandAsideRect(rest, 0.0F, true, false), rest));
  TEST_CHECK(rectNear(islandAsideRect(rest, 0.0F, false, true), rest));

  // Horizontal start island: left by its height, collapsed about its centre.
  TEST_CHECK(rectNear(islandAsideRect(rest, 1.0F, true, false), {200.0F - 32.0F, 20.0F, 0.0F, 0.0F}));
  // Horizontal end island: right.
  TEST_CHECK(rectNear(islandAsideRect(rest, 1.0F, false, false), {200.0F + 32.0F, 20.0F, 0.0F, 0.0F}));

  // Vertical bar: travel is the island's width, along y.
  const bar_detail::IslandRect column{4.0F, 100.0F, 32.0F, 200.0F};
  TEST_CHECK(rectNear(islandAsideRect(column, 1.0F, true, true), {20.0F, 200.0F - 32.0F, 0.0F, 0.0F}));
  TEST_CHECK(rectNear(islandAsideRect(column, 1.0F, false, true), {20.0F, 200.0F + 32.0F, 0.0F, 0.0F}));

  // Halfway: half size, half travel.
  TEST_CHECK(rectNear(islandAsideRect(rest, 0.5F, false, false), {150.0F + 16.0F, 12.0F, 100.0F, 16.0F}));

  // Spring overshoot is clamped.
  TEST_CHECK(rectNear(islandAsideRect(rest, -0.2F, true, false), rest));
  TEST_CHECK(rectNear(islandAsideRect(rest, 1.3F, false, false), islandAsideRect(rest, 1.0F, false, false)));
  return 0;
}
