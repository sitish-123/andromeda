#include "theme/accent_pick.h"

#include "theme/contrast.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace noctalia::theme {

  namespace {

    constexpr std::size_t kHueBins = 36;
    constexpr double kMinSaturation = 0.2;
    constexpr double kMinLightness = 0.1;
    constexpr double kMaxLightness = 0.92;
    // Below this share of the image's pixel weight, the image has no colour worth an accent.
    constexpr double kMinColorShare = 0.01;
    // An accent is a vivid fill: muted picks gain saturation, pale ones lose lightness.
    constexpr double kAccentMinSaturation = 0.55;
    constexpr double kAccentMaxLightness = 0.6;
    constexpr double kWhiteTextRatio = 4.5;

    struct Bin {
      double weight = 0.0;
      double r = 0.0;
      double g = 0.0;
      double b = 0.0;
    };

  } // namespace

  std::optional<Color> pickImageAccent(std::span<const std::uint8_t> rgba) {
    std::array<Bin, kHueBins> bins{};
    double pixels = 0.0;
    for (std::size_t i = 0; i + 3 < rgba.size(); i += 4) {
      if (rgba[i + 3] == 0) {
        continue;
      }
      pixels += 1.0;
      const Color c(rgba[i], rgba[i + 1], rgba[i + 2]);
      const auto [h, s, l] = c.toHsl();
      if (s < kMinSaturation || l < kMinLightness || l > kMaxLightness) {
        continue;
      }
      // Chroma: saturation counts less near black and white.
      const double weight = s * (1.0 - std::abs(2.0 * l - 1.0));
      Bin& bin = bins[static_cast<std::size_t>(h / 360.0 * kHueBins) % kHueBins];
      bin.weight += weight;
      bin.r += weight * c.r;
      bin.g += weight * c.g;
      bin.b += weight * c.b;
    }
    if (pixels == 0.0) {
      return std::nullopt;
    }
    // A hue split across two bins still wins: score each bin with its neighbours.
    std::size_t best = 0;
    double bestScore = -1.0;
    for (std::size_t i = 0; i < kHueBins; ++i) {
      const double score = bins[(i + kHueBins - 1) % kHueBins].weight + 2.0 * bins[i].weight
          + bins[(i + 1) % kHueBins].weight;
      if (score > bestScore) {
        bestScore = score;
        best = i;
      }
    }
    const Bin& bin = bins[best];
    if (bin.weight / pixels < kMinColorShare) {
      return std::nullopt;
    }
    const Color mean(
        static_cast<int>(std::lround(bin.r / bin.weight)), static_cast<int>(std::lround(bin.g / bin.weight)),
        static_cast<int>(std::lround(bin.b / bin.weight))
    );
    const auto [h, s, l] = mean.toHsl();
    return accentForWhiteText(
        Color::fromHsl(h, std::max(s, kAccentMinSaturation), std::min(l, kAccentMaxLightness))
    );
  }

  Color accentForWhiteText(const Color& color) {
    const Color white(255, 255, 255);
    const auto [h, s, l] = color.toHsl();
    Color out = color;
    for (double lightness = l; contrastRatio(out, white) < kWhiteTextRatio && lightness > 0.0;) {
      lightness = std::max(0.0, lightness - 0.01);
      out = Color::fromHsl(h, s, lightness);
    }
    return out;
  }

} // namespace noctalia::theme
