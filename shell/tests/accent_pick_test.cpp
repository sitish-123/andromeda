// The From Wallpaper accent: the image's dominant saturated hue, legible under white text.

#include "tests/test_check.h"
#include "theme/accent_pick.h"
#include "theme/contrast.h"

#include <cstdint>
#include <vector>

namespace {

  using noctalia::theme::Color;

  // A 32x32 image: `mainShare` of it in `main`, the rest split between grey and a little of `minor`.
  std::vector<std::uint8_t> image(Color main, Color minor, int mainRows) {
    std::vector<std::uint8_t> rgba;
    for (int y = 0; y < 32; ++y) {
      for (int x = 0; x < 32; ++x) {
        const Color c = y < mainRows ? main : (x < 4 ? minor : Color(128, 128, 128));
        rgba.insert(rgba.end(), {static_cast<std::uint8_t>(c.r), static_cast<std::uint8_t>(c.g),
                                 static_cast<std::uint8_t>(c.b), 255});
      }
    }
    return rgba;
  }

  double hue(const Color& c) { return std::get<0>(c.toHsl()); }

  void checkLegible(const Color& c) {
    TEST_CHECK(noctalia::theme::contrastRatio(c, Color(255, 255, 255)) >= 4.5);
  }

} // namespace

int main() {
  const Color orange(240, 140, 40);
  const Color sky(90, 170, 240);

  const auto sunset = noctalia::theme::pickImageAccent(image(orange, sky, 12));
  const auto sea = noctalia::theme::pickImageAccent(image(sky, orange, 12));
  TEST_CHECK(sunset.has_value() && sea.has_value());
  checkLegible(*sunset);
  checkLegible(*sea);
  TEST_CHECK(noctalia::theme::hueDistance(hue(*sunset), hue(orange)) < 15.0);
  TEST_CHECK(noctalia::theme::hueDistance(hue(*sea), hue(sky)) < 15.0);

  // A grey picture has no accent.
  TEST_CHECK(!noctalia::theme::pickImageAccent(image(Color(30, 30, 30), Color(200, 200, 200), 16)).has_value());

  checkLegible(noctalia::theme::accentForWhiteText(Color(255, 230, 0)));
  return 0;
}
