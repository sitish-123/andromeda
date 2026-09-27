#include "check.h"
#include "output/mirror.h"

using umbriel::mirrorInputRegion;
using umbriel::mirrorPlacement;

namespace {
  constexpr auto kNormal = WL_OUTPUT_TRANSFORM_NORMAL;
  constexpr auto k90 = WL_OUTPUT_TRANSFORM_90;
} // namespace

UMBRIEL_TEST(equalOutputsFillExactly) {
  const auto p = mirrorPlacement(1920, 1080, kNormal, 1920, 1080, kNormal);
  CHECK(p.dst.x == 0 && p.dst.y == 0 && p.dst.width == 1920 && p.dst.height == 1080);
  CHECK(p.transform == kNormal);
}

UMBRIEL_TEST(widerTargetPillarboxes) {
  // A 16:10 laptop panel on a 16:9 projector keeps its aspect ratio.
  const auto p = mirrorPlacement(2880, 1800, kNormal, 1920, 1080, kNormal);
  CHECK(p.dst.width == 1728 && p.dst.height == 1080);
  CHECK(p.dst.x == 96 && p.dst.y == 0);
}

UMBRIEL_TEST(rotatedSourceIsUprightAndFitsByHeight) {
  const auto p = mirrorPlacement(2880, 1800, k90, 1920, 1080, kNormal);
  CHECK(p.dst.width == 675 && p.dst.height == 1080);
  CHECK(p.dst.x == 622 && p.dst.y == 0);
  CHECK(p.transform == WL_OUTPUT_TRANSFORM_270);
}

UMBRIEL_TEST(rotatedTargetBoxIsInBufferSpace) {
  const auto p = mirrorPlacement(2880, 1800, kNormal, 1920, 1080, k90);
  CHECK(p.dst.width == 675 && p.dst.height == 1080);
  CHECK(p.transform == k90);
}

UMBRIEL_TEST(unconfiguredOutputsYieldEmptyBox) {
  const auto p = mirrorPlacement(0, 0, kNormal, 1920, 1080, kNormal);
  CHECK(p.dst.width == 0 && p.dst.height == 0);
  const auto q = mirrorPlacement(1920, 1080, kNormal, 0, 0, kNormal);
  CHECK(q.dst.width == 0 && q.dst.height == 0);
}

namespace {
  bool boxIs(const wlr_box& box, int x, int y, int width, int height) {
    return box.x == x && box.y == y && box.width == width && box.height == height;
  }
} // namespace

// Ways a touch region on a mirroring panel can go wrong: bars counted as image (offset touches), pixel sizes used
// where the source's logical box belongs (wrong scale), the source's layout position dropped, a division by an empty
// placement, and a linear region pretending to express a rotation.

UMBRIEL_TEST(inputRegionMatchesSourceWithoutBars) {
  CHECK(boxIs(mirrorInputRegion({0, 0, 1920, 1080}, 1920, 1080, kNormal, 3840, 2160, kNormal), 0, 0, 1920, 1080));
}

UMBRIEL_TEST(inputRegionExtendsIntoLetterboxBars) {
  // A 4K TV on the 2880x1800 laptop panel: the image is 2880x1620 at y 90, so the panel spans 120 layout pixels of
  // bar above and below the TV's 3840x2160 box.
  CHECK(boxIs(mirrorInputRegion({0, 0, 3840, 2160}, 3840, 2160, kNormal, 2880, 1800, kNormal), 0, -120, 3840, 2400));
}

UMBRIEL_TEST(inputRegionExtendsIntoPillarboxBars) {
  CHECK(boxIs(mirrorInputRegion({0, 0, 1440, 1080}, 1440, 1080, kNormal, 1920, 1080, kNormal), -240, 0, 1920, 1080));
}

UMBRIEL_TEST(inputRegionUsesTheSourcesLogicalBox) {
  // The TV at scale 2 is 1920x1080 in the layout although its buffer is 3840x2160.
  CHECK(boxIs(mirrorInputRegion({0, 0, 1920, 1080}, 3840, 2160, kNormal, 2880, 1800, kNormal), 0, -60, 1920, 1200));
}

UMBRIEL_TEST(inputRegionFollowsTheSourcesPosition) {
  CHECK(
      boxIs(mirrorInputRegion({1000, 500, 3840, 2160}, 3840, 2160, kNormal, 2880, 1800, kNormal), 1000, 380, 3840, 2400)
  );
}

UMBRIEL_TEST(inputRegionIsEmptyForUnconfiguredOutputs) {
  const wlr_box none = mirrorInputRegion({0, 0, 1920, 1080}, 0, 0, kNormal, 2880, 1800, kNormal);
  CHECK(none.width == 0 && none.height == 0);
  const wlr_box noTarget = mirrorInputRegion({0, 0, 1920, 1080}, 1920, 1080, kNormal, 0, 0, kNormal);
  CHECK(noTarget.width == 0 && noTarget.height == 0);
}

UMBRIEL_TEST(inputRegionFallsBackToSourceBoxWhenRotated) {
  CHECK(boxIs(mirrorInputRegion({0, 0, 1920, 1080}, 1920, 1080, k90, 2880, 1800, kNormal), 0, 0, 1920, 1080));
  CHECK(boxIs(mirrorInputRegion({0, 0, 1920, 1080}, 1920, 1080, kNormal, 2880, 1800, k90), 0, 0, 1920, 1080));
}

int main() { return RUN_TESTS(); }
