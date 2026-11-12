#include "shell/live_activity/island_banner_swipe.h"
#include "tests/test_check.h"

#include <cmath>

namespace {

  using enum BannerSwipeAxis;

  void testSlopThenAxisStays() {
    TEST_CHECK(bannerSwipeAxis(Undecided, 5.0F, -6.0F) == Undecided);
    TEST_CHECK(bannerSwipeAxis(Undecided, 12.0F, 3.0F) == Horizontal);
    TEST_CHECK(bannerSwipeAxis(Undecided, 2.0F, -12.0F) == Vertical);
    TEST_CHECK(bannerSwipeAxis(Horizontal, 0.0F, -80.0F) == Horizontal);
  }

  void testSwipeRightFollowsAndDismisses() {
    const float limit = 700.0F;
    TEST_CHECK(bannerSwipeOffset(120.0F, limit) == 120.0F);
    TEST_CHECK(bannerSwipeOffset(900.0F, limit) == limit);
    TEST_CHECK(std::abs(bannerSwipeOffset(-100.0F, limit) + 30.0F) < 1e-3F);
    // Past half the travel at rest, or a slow drag that a flick carries past it.
    TEST_CHECK(bannerSwipeDismisses(400.0F, 0.0F, limit));
    TEST_CHECK(!bannerSwipeDismisses(120.0F, 0.0F, limit));
    TEST_CHECK(bannerSwipeDismisses(60.0F, 0.6F, limit));
    TEST_CHECK(!bannerSwipeDismisses(300.0F, -1.0F, limit));
  }

  void testSwipeUpDismissesAndDownRubberBands() {
    const float limit = -90.0F;
    TEST_CHECK(bannerSwipeOffset(-40.0F, limit) == -40.0F);
    TEST_CHECK(bannerSwipeOffset(-200.0F, limit) == limit);
    TEST_CHECK(std::abs(bannerSwipeOffset(50.0F, limit) - 15.0F) < 1e-3F);
    TEST_CHECK(bannerSwipeDismisses(-50.0F, 0.0F, limit));
    TEST_CHECK(!bannerSwipeDismisses(-20.0F, 0.0F, limit));
    TEST_CHECK(!bannerSwipeDismisses(10.0F, 0.0F, limit));
  }

} // namespace

int main() {
  testSlopThenAxisStays();
  testSwipeRightFollowsAndDismisses();
  testSwipeUpDismissesAndDownRubberBands();
  return 0;
}
