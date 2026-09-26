#include "view/presented_crop.h"

#include "check.h"

UMBRIEL_TEST(fullscreenCentersASmallerBuffer) {
  // A client that has not yet committed the fullscreen size sits centered in the
  // tile rather than pinned to a corner.
  CHECK_EQ(umbriel::fullscreenCenterOffset(1920, 1280), 320);
  CHECK_EQ(umbriel::fullscreenCenterOffset(1080, 800), 140);
}

UMBRIEL_TEST(fullscreenCropsAnOversizedBufferEqually) {
  // Negative offset: the buffer is wider than the output, so it loses the same
  // amount from each side instead of being scaled.
  CHECK_EQ(umbriel::fullscreenCenterOffset(1920, 2560), -320);
}

UMBRIEL_TEST(fullscreenOffsetIsZeroWithoutGeometry) {
  // A client with no committed geometry must not be shoved half an output over.
  CHECK_EQ(umbriel::fullscreenCenterOffset(1920, 0), 0);
  CHECK_EQ(umbriel::fullscreenCenterOffset(1920, -1), 0);
}

UMBRIEL_TEST(cropSelectsTheWholeBufferWhenNothingIsHidden) {
  const wlr_fbox base{0, 0, 1200, 900};
  const wlr_box geometry{0, 0, 1200, 900};
  // Presented at the committed size, fully visible: the source is the whole buffer.
  const wlr_box content{0, 0, 1200, 900};
  const wlr_box clip{0, 0, 1200, 900};

  const wlr_fbox src = umbriel::croppedSourceBox(base, geometry, content, clip, 1200, 900);

  CHECK_EQ(static_cast<int>(src.x), 0);
  CHECK_EQ(static_cast<int>(src.y), 0);
  CHECK_EQ(static_cast<int>(src.width), 1200);
  CHECK_EQ(static_cast<int>(src.height), 900);
}

UMBRIEL_TEST(cropScalesTheSourceByThePresentedShrink) {
  // Animating down to half size: each presented pixel covers two buffer pixels,
  // so a full-width visible box still selects the full-width source.
  const wlr_fbox base{0, 0, 1200, 900};
  const wlr_box geometry{0, 0, 1200, 900};
  const wlr_box content{0, 0, 600, 450};
  const wlr_box clip{0, 0, 600, 450};

  const wlr_fbox src = umbriel::croppedSourceBox(base, geometry, content, clip, 1200, 900);

  CHECK_EQ(static_cast<int>(src.width), 1200);
  CHECK_EQ(static_cast<int>(src.height), 900);
}

UMBRIEL_TEST(cropSelectsOnlyTheVisibleHalf) {
  // Presented at half size with only the left half on this output.
  const wlr_fbox base{0, 0, 1200, 900};
  const wlr_box geometry{0, 0, 1200, 900};
  const wlr_box content{0, 0, 600, 450};
  const wlr_box clip{0, 0, 300, 450};

  const wlr_fbox src = umbriel::croppedSourceBox(base, geometry, content, clip, 1200, 900);

  CHECK_EQ(static_cast<int>(src.x), 0);
  CHECK_EQ(static_cast<int>(src.width), 600);
}

UMBRIEL_TEST(cropHonoursTheSurfaceViewport) {
  // A client with a viewport: the buffer is twice the surface size, so every
  // surface coordinate maps to two buffer pixels on top of the presented scale.
  const wlr_fbox base{0, 0, 2400, 1800};
  const wlr_box geometry{0, 0, 1200, 900};
  const wlr_box content{0, 0, 1200, 900};
  const wlr_box clip{0, 0, 600, 900};

  const wlr_fbox src = umbriel::croppedSourceBox(base, geometry, content, clip, 1200, 900);

  CHECK_EQ(static_cast<int>(src.width), 1200);
  CHECK_EQ(static_cast<int>(src.height), 1800);
}

UMBRIEL_TEST(cropNeverReachesOutsideTheBuffer) {
  // A clip extending past the surface must clamp, not sample garbage.
  const wlr_fbox base{0, 0, 1200, 900};
  const wlr_box geometry{0, 0, 1200, 900};
  const wlr_box content{0, 0, 1200, 900};
  const wlr_box clip{600, 0, 1200, 900};

  const wlr_fbox src = umbriel::croppedSourceBox(base, geometry, content, clip, 1200, 900);

  CHECK(src.x >= base.x);
  CHECK(src.x + src.width <= base.x + base.width);
  CHECK(src.y + src.height <= base.y + base.height);
}

UMBRIEL_TEST(cropIsEmptyForDegenerateInput) {
  const wlr_fbox base{0, 0, 1200, 900};
  const wlr_box geometry{0, 0, 1200, 900};
  const wlr_box clip{0, 0, 100, 100};

  // A zero-width presented box would divide by zero.
  CHECK(umbriel::croppedSourceBox(base, geometry, {0, 0, 0, 900}, clip, 1200, 900).width <= 0);
  // A surface that has committed nothing yet.
  CHECK(umbriel::croppedSourceBox(base, geometry, {0, 0, 600, 450}, clip, 0, 0).width <= 0);
  // A client with no geometry.
  CHECK(umbriel::croppedSourceBox(base, {0, 0, 0, 0}, {0, 0, 600, 450}, clip, 1200, 900).width <= 0);
}

int main() { return RUN_TESTS(); }
