// The damage region must cover every changed rect, and a reused buffer must be repainted with the damage of every
// frame it missed.

#include "render/core/damage_region.h"
#include "render/scene/text_node.h"
#include "tests/test_check.h"

#include <vector>

namespace {

  bool covered(const DamageRegion& region, const DamageRect& rect) {
    for (const DamageRect& r : region.rects()) {
      if (r.contains(rect)) {
        return true;
      }
    }
    return false;
  }

  void checkShape(const DamageRegion& region) {
    const auto rects = region.rects();
    TEST_CHECK(rects.size() <= DamageRegion::kMaxRects);
    for (std::size_t i = 0; i < rects.size(); ++i) {
      for (std::size_t j = i + 1; j < rects.size(); ++j) {
        TEST_CHECK(!rects[i].overlaps(rects[j]));
      }
    }
  }

} // namespace

int main() {
  DamageRegion region;
  std::vector<DamageRect> added;
  for (int i = 0; i < 20; ++i) {
    const float x = static_cast<float>((i * 97) % 1200);
    const float y = static_cast<float>((i * 53) % 700);
    const DamageRect rect{x, y, x + 40.0F + static_cast<float>(i), y + 20.0F};
    region.add(rect);
    added.push_back(rect);
    checkShape(region);
    for (const DamageRect& r : added) {
      TEST_CHECK(covered(region, r));
    }
  }

  DamageRegion apart;
  apart.add({0.0F, 0.0F, 60.0F, 20.0F});
  apart.add({1200.0F, 0.0F, 1260.0F, 20.0F});
  TEST_CHECK(apart.rects().size() == 2);

  DamageRegion clipped;
  clipped.add({-10.0F, -10.0F, 5.0F, 5.0F});
  clipped.add({50.0F, 50.0F, 60.0F, 60.0F});
  clipped.clipTo({0.0F, 0.0F, 40.0F, 40.0F});
  TEST_CHECK(clipped.rects().size() == 1);
  TEST_CHECK((clipped.rects()[0] == DamageRect{0.0F, 0.0F, 5.0F, 5.0F}));

  DamageHistory history;
  DamageRegion first;
  first.add({0.0F, 0.0F, 10.0F, 10.0F});
  DamageRegion second;
  second.add({100.0F, 0.0F, 110.0F, 10.0F});
  DamageRegion current;
  current.add({200.0F, 0.0F, 210.0F, 10.0F});

  TEST_CHECK(history.regionFor(1, current).has_value());
  TEST_CHECK(!history.regionFor(2, current).has_value());
  history.push(first);
  history.push(second);

  const auto age1 = history.regionFor(1, current);
  TEST_CHECK(age1.has_value() && age1->rects().size() == 1);
  const auto age3 = history.regionFor(3, current);
  TEST_CHECK(age3.has_value());
  TEST_CHECK(covered(*age3, first.rects()[0]) && covered(*age3, second.rects()[0]) && covered(*age3, current.rects()[0]));
  const auto age2 = history.regionFor(2, current);
  TEST_CHECK(age2.has_value() && covered(*age2, second.rects()[0]) && !covered(*age2, first.rects()[0]));
  TEST_CHECK(!history.regionFor(0, current).has_value());
  TEST_CHECK(!history.regionFor(4, current).has_value());

  history.reset();
  TEST_CHECK(!history.regionFor(2, current).has_value());

  // New text with the same bounds ("1" -> "2" in a badge) must still repaint the node.
  TextNode text;
  text.setText("1");
  text.clearDirty();
  text.setText("2");
  TEST_CHECK(text.selfPaintDirty());
  return 0;
}
