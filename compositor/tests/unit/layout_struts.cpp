#include "check.h"
#include "layout/layout.h"

extern "C" {
#include <wlr/util/box.h>
}

using umbriel::LayoutStruts;

UMBRIEL_TEST(positiveStrutsInsetAnOffsetArea) {
  const wlr_box area{.x = 100, .y = 50, .width = 1000, .height = 700};
  const wlr_box result =
      umbriel::applyLayoutStruts(area, LayoutStruts{.left = 10, .right = 20, .top = 30, .bottom = 40});

  CHECK_EQ(result.x, 110);
  CHECK_EQ(result.y, 80);
  CHECK_EQ(result.width, 970);
  CHECK_EQ(result.height, 630);
}

UMBRIEL_TEST(negativeStrutsExpandAnArea) {
  const wlr_box area{.x = 100, .y = 50, .width = 1000, .height = 700};
  const wlr_box result =
      umbriel::applyLayoutStruts(area, LayoutStruts{.left = -10, .right = -20, .top = -30, .bottom = -40});

  CHECK_EQ(result.x, 90);
  CHECK_EQ(result.y, 20);
  CHECK_EQ(result.width, 1030);
  CHECK_EQ(result.height, 770);
}

UMBRIEL_TEST(overlappingStrutsClampTheExtentToZero) {
  const wlr_box area{.x = 0, .y = 0, .width = 1280, .height = 720};
  const wlr_box result =
      umbriel::applyLayoutStruts(area, LayoutStruts{.left = 900, .right = 900, .top = 500, .bottom = 500});

  CHECK_EQ(result.x, 900);
  CHECK_EQ(result.y, 500);
  CHECK_EQ(result.width, 0);
  CHECK_EQ(result.height, 0);
}

int main() { return RUN_TESTS(); }
