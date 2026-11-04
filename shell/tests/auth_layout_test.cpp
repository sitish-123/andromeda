#include "tests/test_check.h"
#include "ui/auth/auth_layout.h"

#include <cmath>

namespace {

  bool near(float a, float b) { return std::abs(a - b) <= 4.0F; }

  // Top of the account column (avatar), or of the reason card above it.
  float accountTop(const AuthPlacement& p, float s, bool reasonCard) {
    return p.capsule.y - 112.0F * s - (reasonCard ? 100.0F * s : 0.0F);
  }

  float contentBottom(const AuthPlacement& p) {
    float bottom = p.clock ? p.clockY + p.clockSize : 0.0F;
    if (!p.media.empty()) {
      bottom = p.media.bottom();
    }
    if (!p.chips.empty()) {
      bottom = p.chips.bottom();
    }
    return bottom;
  }

} // namespace

// Boxes against .agent/mockups/lockscreen.html section 1 (1280 x 800, scale 1), within 4 px.
int main() {
  {
    const AuthPlacement p = placeAuth({.sw = 1280, .sh = 800, .scale = 1, .media = true, .chips = true});
    TEST_CHECK(near(p.date.y, 172));
    TEST_CHECK(near(p.clockY, 200));
    TEST_CHECK(near(p.media.x, 460) && near(p.media.y, 300) && near(p.media.w, 360));
    TEST_CHECK(near(p.chips.y, 372));
    TEST_CHECK(near(accountTop(p, 1, false), 468));
    TEST_CHECK(near(p.capsule.x, 520) && near(p.capsule.w, 240) && near(p.capsule.h, 40));
    TEST_CHECK(near(p.sessionButton.right(), 1256) && near(p.sessionButton.bottom(), 776));
  }
  {
    const AuthPlacement p = placeAuth({.layout = AuthLayout::Glance, .sw = 1280, .sh = 800, .scale = 1, .cards = 4});
    TEST_CHECK(!p.clockCentred && near(p.clockX, 96));
    TEST_CHECK(near(p.date.y, 120));
    TEST_CHECK(near(p.cards.x, 96) && near(p.cards.y, 252) && near(p.cards.w, 400));
    TEST_CHECK(near(p.glanceHintY + 16, 728));
    TEST_CHECK(near(p.sheet.w, 360) && near(p.sheet.x + p.sheet.w * 0.5F, 640) && near(p.sheet.y + p.sheet.h * 0.5F, 400));
  }
  {
    const AuthPlacement p = placeAuth({.layout = AuthLayout::Split, .sw = 1280, .sh = 800, .scale = 1, .cards = 3});
    TEST_CHECK(p.layout == AuthLayout::Split);
    TEST_CHECK(near(p.date.y, 298) && near(p.clockY, 326) && near(p.clockSize, 96));
    TEST_CHECK(near(p.cards.y, 446) && near(p.cards.w, 352));
    TEST_CHECK(near(accountTop(p, 1, false), 310));
    TEST_CHECK(near(p.capsule.right(), 1120));
  }
  {
    const AuthPlacement idle = placeAuth({.layout = AuthLayout::Minimal, .sw = 1280, .sh = 800, .scale = 1});
    TEST_CHECK(near(idle.clockY, 320) && near(idle.clockSize, 96));
    TEST_CHECK(near(idle.date.y, 424) && idle.dateSecondary);
    TEST_CHECK(near(idle.capsule.bottom(), 704));
    TEST_CHECK(!idle.account && !idle.chrome);
    TEST_CHECK(placeAuth({.layout = AuthLayout::Minimal, .sw = 1280, .sh = 800, .scale = 1, .pointerMoved = true}).chrome);
  }

  // Any ui_scale: the content above never runs into the account column; the column itself never moves.
  for (const float s : {1.0F, 1.5F, 2.0F}) {
    for (const bool reason : {false, true}) {
      const AuthPlacement p =
          placeAuth({.sw = 1280, .sh = 800, .scale = s, .media = true, .chips = true, .reasonCard = reason});
      TEST_CHECK(contentBottom(p) <= accountTop(p, s, reason));
      TEST_CHECK(!p.clock || p.date.y >= 40.0F * s);
      TEST_CHECK(near(p.capsule.y + p.capsule.h * 0.5F, 600));
    }
  }
  {
    // Short of room, the chips go before the media card; with less room, both go.
    const AuthPlacement mid = placeAuth({.sw = 1280, .sh = 840, .scale = 1.5F, .media = true, .chips = true});
    TEST_CHECK(!mid.media.empty() && mid.chips.empty());
    const AuthPlacement big = placeAuth({.sw = 1280, .sh = 800, .scale = 2, .media = true, .chips = true});
    TEST_CHECK(big.media.empty() && big.chips.empty() && !big.clock);
  }
  {
    // A ringing call takes room above the account like the reason card; its card ends 32 above the avatar.
    const float block = 32 + 16 + 48 + 16 + 40 + 16;
    const AuthPlacement p =
        placeAuth({.sw = 1280, .sh = 800, .scale = 1, .media = true, .chips = true, .callBlock = block});
    TEST_CHECK(near(p.cardBottom, accountTop(p, 1, false) - 32));
    TEST_CHECK(contentBottom(p) <= accountTop(p, 1, false) - block);
    const AuthPlacement minimal =
        placeAuth({.layout = AuthLayout::Minimal, .sw = 1280, .sh = 800, .scale = 1, .callBlock = block});
    TEST_CHECK(near(minimal.cardBottom, minimal.capsule.y - 32));
  }
  {
    // Split with no room beside the column falls back to Centered.
    const AuthPlacement p = placeAuth({.layout = AuthLayout::Split, .sw = 1024, .sh = 640, .scale = 1.5F, .cards = 3});
    TEST_CHECK(p.layout == AuthLayout::Centered);
  }
  TEST_CHECK(authLayoutFromName("split") == AuthLayout::Split);
  TEST_CHECK(authLayoutFromName("custom") == AuthLayout::Centered);
  return 0;
}
