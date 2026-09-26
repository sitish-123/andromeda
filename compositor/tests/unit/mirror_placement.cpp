#include "check.h"
#include "output/mirror.h"

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

int main() { return RUN_TESTS(); }
