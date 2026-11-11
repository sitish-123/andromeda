#include "shell/live_activity/island_lane_layout.h"
#include "tests/test_check.h"

namespace {

  constexpr float kCap = 56.0F;

  void testPillIsBarWideAtAnyScale() {
    // A 32 px bar at scale 1 and the same bar at 1.5 (48 logical px) both give a 32 layout px pill.
    TEST_CHECK(island_lane::pillWidth(32.0F, 1.0F, kCap) == 32.0F);
    TEST_CHECK(island_lane::pillWidth(48.0F, 1.5F, kCap) == 32.0F);
    TEST_CHECK(island_lane::pillWidth(40.0F, 1.0F, kCap) == 40.0F);
  }

  void testPillHugsItsContent() {
    // Caption first, count last: 12 at both text ends.
    const auto idle = island_lane::pillLength(70.0F, true, true);
    TEST_CHECK(idle.height == 94.0F && idle.padTop == 12.0F);
    // Art first, bars last: 8 at both symbol ends.
    const auto media = island_lane::pillLength(46.0F, false, false);
    TEST_CHECK(media.height == 62.0F && media.padTop == 8.0F);
    // Phone glyph first, the stacked time last.
    const auto call = island_lane::pillLength(48.0F, false, true);
    TEST_CHECK(call.height == 68.0F && call.padTop == 8.0F);
  }

  void testPillIsCentredInTheLane() {
    const float w = island_lane::surfaceWidth(10.0F, 32.0F, 360.0F);
    TEST_CHECK(w == 410.0F);
    const auto left = island_lane::pill(w, 280.0F, 10.0F, 32.0F, 62.0F, false);
    TEST_CHECK(left.x == 10.0F && left.w == 32.0F && left.h == 62.0F);
    TEST_CHECK(left.y + left.h * 0.5F == 140.0F);
    const auto right = island_lane::pill(w, 280.0F, 10.0F, 32.0F, 62.0F, true);
    TEST_CHECK(right.x + right.w == w - 10.0F && right.y == left.y);
  }

  void testCardGrowsSidewaysTopEdgesEqual() {
    const auto pill = island_lane::pill(410.0F, 280.0F, 10.0F, 32.0F, 62.0F, false);
    const auto card = island_lane::card(pill, 280.0F, 360.0F, 136.0F, false);
    TEST_CHECK(card.x == pill.x + pill.w + 8.0F && card.y == pill.y);
    const auto rightPill = island_lane::pill(410.0F, 280.0F, 10.0F, 32.0F, 62.0F, true);
    const auto rightCard = island_lane::card(rightPill, 280.0F, 360.0F, 136.0F, true);
    TEST_CHECK(rightCard.x + rightCard.w == rightPill.x - 8.0F && rightCard.y == rightPill.y);
    TEST_CHECK(rightCard.x >= 0.0F);
  }

} // namespace

int main() {
  testPillIsBarWideAtAnyScale();
  testPillHugsItsContent();
  testPillIsCentredInTheLane();
  testCardGrowsSidewaysTopEdgesEqual();
  return 0;
}
