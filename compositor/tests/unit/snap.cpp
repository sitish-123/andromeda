#include "view/snap.h"

#include "check.h"

extern "C" {
#include <wlr/util/edges.h>
}

using namespace andromeda;

ANDROMEDA_TEST(halvesShareABoundary) {
  const wlr_box area{8, 8, 1264, 704};
  const auto left = snapRegionBox(area, SnapRegion::Left, 8);
  const auto right = snapRegionBox(area, SnapRegion::Right, 8);
  CHECK_EQ(left.x, 8);
  CHECK_EQ(left.width, 628);
  CHECK_EQ(right.x, 644);
  CHECK_EQ(right.width, 628);
  CHECK_EQ(left.height, 704);
}

ANDROMEDA_TEST(oddWidthGivesTheRemainderRight) {
  CHECK_EQ(snapRegionBox({8, 8, 1265, 704}, SnapRegion::Left, 8).width, 628);
  CHECK_EQ(snapRegionBox({8, 8, 1265, 704}, SnapRegion::Right, 8).width, 629);
}

ANDROMEDA_TEST(quartersSplitBothAxes) {
  const auto box = snapRegionBox({8, 8, 1264, 704}, SnapRegion::BottomRight, 8);
  CHECK_EQ(box.x, 644);
  CHECK_EQ(box.y, 364);
  CHECK_EQ(box.width, 628);
  CHECK_EQ(box.height, 348);
}

ANDROMEDA_TEST(cornerBeatsEdge) {
  CHECK(snapZoneAt({0, 0, 1280, 720}, 2, 40, 0) == SnapRegion::TopLeft);
  CHECK(snapZoneAt({0, 0, 1280, 720}, 1279, 719, 0) == SnapRegion::BottomRight);
}

ANDROMEDA_TEST(verticalEdgesSnapToHalves) {
  CHECK(snapZoneAt({0, 0, 1280, 720}, 640, 0, 0) == SnapRegion::Top);
  CHECK(snapZoneAt({0, 0, 1280, 720}, 640, 719, 0) == SnapRegion::Bottom);
  CHECK(!snapZoneAt({0, 0, 1280, 720}, 640, 719, WLR_EDGE_BOTTOM));
  CHECK(!snapZoneAt({0, 0, 1280, 720}, 640, 0, WLR_EDGE_TOP));
}

ANDROMEDA_TEST(verticalHalvesKeepFullWidthAndGiveRemainderToBottom) {
  const auto top = snapRegionBox({-600, 20, 1000, 705}, SnapRegion::Top, 8);
  const auto bottom = snapRegionBox({-600, 20, 1000, 705}, SnapRegion::Bottom, 8);
  CHECK_EQ(top.x, -600);
  CHECK_EQ(top.y, 20);
  CHECK_EQ(top.width, 1000);
  CHECK_EQ(top.height, 348);
  CHECK_EQ(bottom.x, -600);
  CHECK_EQ(bottom.y, 376);
  CHECK_EQ(bottom.width, 1000);
  CHECK_EQ(bottom.height, 349);
}

ANDROMEDA_TEST(closedEdgesNeverSnap) {
  CHECK(!snapZoneAt({0, 0, 1280, 720}, 1279, 360, WLR_EDGE_RIGHT));
  CHECK(!snapZoneAt({0, 0, 1280, 720}, 2, 2, WLR_EDGE_LEFT | WLR_EDGE_TOP));
}

ANDROMEDA_TEST(regionsUseLogicalCoordinatesOnNegativeOutputs) {
  CHECK(snapZoneAt({-1024, -576, 1024, 576}, -1023, -300, 0) == SnapRegion::Left);
  CHECK(!snapZoneAt({-1024, -576, 1024, 576}, 0, -300, 0));
}

ANDROMEDA_TEST(regionNamesRoundTrip) {
  for (const auto name : {"left", "right", "top", "bottom", "top_left", "top_right", "bottom_left", "bottom_right"}) {
    const auto region = snapRegionNamed(name);
    CHECK(region.has_value());
    if (region)
      CHECK_EQ(snapRegionName(*region), name);
  }
  CHECK(!snapRegionNamed("top-left"));
  CHECK(!snapRegionNamed(""));
}

int main() { return RUN_TESTS(); }

ANDROMEDA_TEST(snapLayoutZonesTileTheAreaWithEvenGaps) {
  const wlr_box area{.x = 10, .y = 20, .width = 1200, .height = 800};
  // Thirds with a 12 px gap: zones 12 px apart, flush with both area edges.
  const wlr_box first = andromeda::snapZoneBox(area, {.layout = 2, .zone = 0}, 12);
  const wlr_box middle = andromeda::snapZoneBox(area, {.layout = 2, .zone = 1}, 12);
  const wlr_box last = andromeda::snapZoneBox(area, {.layout = 2, .zone = 2}, 12);
  CHECK(first.x == 10 && middle.x == first.x + first.width + 12 && last.x == middle.x + middle.width + 12);
  CHECK(last.x + last.width == 1210 && first.height == 800);
  // 1 + 2 stacked: the right zones split the height with the gap between them.
  const wlr_box upper = andromeda::snapZoneBox(area, {.layout = 3, .zone = 1}, 12);
  const wlr_box lower = andromeda::snapZoneBox(area, {.layout = 3, .zone = 2}, 12);
  CHECK(upper.y == 20 && lower.y == upper.y + upper.height + 12 && lower.y + lower.height == 820);
}

ANDROMEDA_TEST(snapSpotNamesRoundTrip) {
  const auto zone = andromeda::snapSpotNamed("6.3");
  CHECK(zone.has_value() && andromeda::snapSpotName(*zone) == "6.3");
  CHECK(andromeda::snapSpotNamed("left").has_value());
  CHECK(!andromeda::snapSpotNamed("1.3").has_value()); // halves have two zones
  CHECK(!andromeda::snapSpotNamed("7.1").has_value());
  CHECK(!andromeda::snapSpotNamed("0.1").has_value());
}
