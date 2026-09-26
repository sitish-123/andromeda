#include "view/border_ring.h"

#include "check.h"

using umbriel::makeBorderRing;
using umbriel::nestedRadius;

UMBRIEL_TEST(nestedRadiusStaysRounded) {
  CHECK_EQ(nestedRadius(0, 4), 0);
  CHECK_EQ(nestedRadius(4, 4), 2);
  CHECK_EQ(nestedRadius(1, 9), 1);
  CHECK_EQ(nestedRadius(10, 4), 7);
}

UMBRIEL_TEST(roundedRingIncludesRasterMargin) {
  constexpr int kWidth = 200;
  constexpr int kHeight = 120;
  constexpr int kThickness = 4;
  constexpr int kExtent = kThickness + 1;
  const auto ring = makeBorderRing(kWidth, kHeight, 10, kThickness, 0);

  CHECK_EQ(ring.box.x, -kExtent);
  CHECK_EQ(ring.box.y, -kExtent);
  CHECK_EQ(ring.box.width, kWidth + 2 * kExtent);
  CHECK_EQ(ring.box.height, kHeight + 2 * kExtent);
}

UMBRIEL_TEST(ringHoleMatchesTheWindow) {
  constexpr int kWidth = 200;
  constexpr int kHeight = 120;
  constexpr int kThickness = 4;
  constexpr int kExtent = kThickness + 1;
  const auto ring = makeBorderRing(kWidth, kHeight, 10, kThickness, 0);

  CHECK_EQ(ring.hole.x, kExtent);
  CHECK_EQ(ring.hole.y, kExtent);
  CHECK_EQ(ring.hole.width, kWidth);
  CHECK_EQ(ring.hole.height, kHeight);
}

UMBRIEL_TEST(ringUsesSmoothNestedRadii) {
  constexpr int kRadius = 8;
  const auto ring = makeBorderRing(200, 120, kRadius, 1, 8);

  CHECK_EQ(static_cast<int>(ring.outer.top_left), kRadius);
  CHECK_EQ(static_cast<int>(ring.seam.top_right), 4);
  CHECK_EQ(static_cast<int>(ring.inner.bottom_right), 4);
  CHECK_EQ(static_cast<int>(ring.outer.bottom_left), kRadius);
}

UMBRIEL_TEST(squareRingKeepsSquareOuterEdge) {
  const auto ring = makeBorderRing(200, 120, 0, 4, 0);

  CHECK_EQ(static_cast<int>(ring.outer.top_left), 0);
  CHECK_EQ(static_cast<int>(ring.outer.bottom_right), 0);
  CHECK_EQ(ring.box.x, -5);
  CHECK_EQ(ring.hole.x, 5);
}

int main() { return RUN_TESTS(); }
