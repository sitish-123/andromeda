// A rounded rect's region (the bar islands' blur and input region, docs/architecture-v2.md §7) covers every pixel
// fully inside the shape and no pixel fully outside it, for random sizes and per-corner radii.
#include "tests/test_check.h"
#include "wayland/surface.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <random>
#include <vector>

namespace {

  constexpr float kEps = 1e-3F;

  struct Shape {
    float w;
    float h;
    // Top-left, top-right, bottom-right, bottom-left.
    std::array<float, 4> r;

    // Signed distance outside the shape, measured only inside the corner squares.
    [[nodiscard]] float outside(float px, float py) const {
      if (px < 0.0F || py < 0.0F || px > w || py > h) {
        return 1.0F;
      }
      const std::array<std::array<float, 2>, 4> centers{{
          {r[0], r[0]},
          {w - r[1], r[1]},
          {w - r[2], h - r[2]},
          {r[3], h - r[3]},
      }};
      const std::array<std::array<float, 2>, 4> sign{{{-1, -1}, {1, -1}, {1, 1}, {-1, 1}}};
      for (std::size_t i = 0; i < 4; ++i) {
        const float dx = (px - centers[i][0]) * sign[i][0];
        const float dy = (py - centers[i][1]) * sign[i][1];
        if (dx > 0.0F && dy > 0.0F) {
          return std::hypot(dx, dy) - r[i];
        }
      }
      return 0.0F;
    }
  };

  void checkRegion(int x, int y, int w, int h, std::array<float, 4> radii, int stripPx) {
    const auto rects = Surface::tessellateRoundedRect(x, y, w, h, radii[0], radii[1], radii[2], radii[3], stripPx);
    const float maxRadius = std::min(w, h) * 0.5F;
    Shape shape{static_cast<float>(w), static_cast<float>(h), radii};
    for (float& r : shape.r) {
      r = std::clamp(r, 0.0F, maxRadius);
    }

    std::vector<bool> covered(static_cast<std::size_t>(w * h), false);
    for (const InputRect& rect : rects) {
      TEST_CHECK(rect.width > 0 && rect.height > 0);
      TEST_CHECK(rect.x >= x && rect.y >= y && rect.x + rect.width <= x + w && rect.y + rect.height <= y + h);
      for (int py = rect.y - y; py < rect.y - y + rect.height; ++py) {
        for (int px = rect.x - x; px < rect.x - x + rect.width; ++px) {
          covered[static_cast<std::size_t>(py * w + px)] = true;
        }
      }
    }

    for (int py = 0; py < h; ++py) {
      for (int px = 0; px < w; ++px) {
        float worst = -1.0F;
        float best = 1.0F;
        for (const auto [cx, cy] : std::array<std::array<int, 2>, 4>{{{0, 0}, {1, 0}, {0, 1}, {1, 1}}}) {
          const float d = shape.outside(static_cast<float>(px + cx), static_cast<float>(py + cy));
          worst = std::max(worst, d);
          best = std::min(best, d);
        }
        const bool isCovered = covered[static_cast<std::size_t>(py * w + px)];
        if (worst <= -kEps) {
          TEST_CHECK(isCovered);
        }
        if (stripPx == 1 && best >= kEps) {
          TEST_CHECK(!isCovered);
        }
      }
    }
  }

} // namespace

int main() {
  std::mt19937 rng(0x5eedU);
  std::uniform_int_distribution<int> size(1, 90);
  std::uniform_int_distribution<int> origin(-20, 20);
  std::uniform_int_distribution<int> strip(1, 4);
  std::uniform_real_distribution<float> unit(0.0F, 1.2F);

  for (int i = 0; i < 4000; ++i) {
    const int w = size(rng);
    const int h = size(rng);
    const float half = std::min(w, h) * 0.5F;
    const int stripPx = i % 2 == 0 ? 1 : strip(rng);
    if (i % 4 == 0) {
      checkRegion(origin(rng), origin(rng), w, h, {half, half, half, half}, stripPx);
      continue;
    }
    checkRegion(
        origin(rng), origin(rng), w, h, {unit(rng) * half, unit(rng) * half, unit(rng) * half, unit(rng) * half},
        stripPx
    );
  }
  return 0;
}
