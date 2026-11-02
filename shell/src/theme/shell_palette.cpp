#include "theme/shell_palette.h"

#include "theme/color.h"
#include "theme/contrast.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <span>

namespace noctalia::theme {

  namespace {

    constexpr double kMinContrast = 4.5;

    int byte(float v) { return std::clamp(static_cast<int>(std::lround(v * 255.0F)), 0, 255); }

    Color toInt(const ::Color& c) { return Color(byte(c.r), byte(c.g), byte(c.b)); }

    ::Color toUi(const Color& c, float alpha = 1.0F) {
      return ::Color{
          .r = static_cast<float>(c.r) / 255.0F,
          .g = static_cast<float>(c.g) / 255.0F,
          .b = static_cast<float>(c.b) / 255.0F,
          .a = alpha,
      };
    }

    ::Color opaque(const ::Color& c) { return ::Color{.r = c.r, .g = c.g, .b = c.b, .a = 1.0F}; }

    Color composite(const ::Color& fg, const ::Color& bg) {
      return toInt(::Color{
          .r = fg.r * fg.a + bg.r * (1.0F - fg.a),
          .g = fg.g * fg.a + bg.g * (1.0F - fg.a),
          .b = fg.b * fg.a + bg.b * (1.0F - fg.a),
          .a = 1.0F,
      });
    }

    bool readsOnAll(const ::Color& label, std::span<const ::Color> planes) {
      return std::ranges::all_of(planes, [&](const ::Color& plane) {
        return contrastRatio(composite(label, plane), toInt(plane)) >= kMinContrast;
      });
    }

    // Raises the label's opacity first, then moves its lightness, until it reads on every plane.
    ::Color clampLabel(::Color label, std::span<const ::Color> planes) {
      for (float alpha = label.a; alpha < 1.0F; alpha += 0.02F) {
        label.a = alpha;
        if (readsOnAll(label, planes)) {
          return label;
        }
      }
      label.a = 1.0F;
      Color rgb = toInt(label);
      for (int pass = 0; pass < 2 && !readsOnAll(toUi(rgb), planes); ++pass) {
        for (const ::Color& plane : planes) {
          rgb = ensureContrast(rgb, toInt(plane), kMinContrast);
        }
      }
      return toUi(rgb);
    }

    std::optional<::Color> role(const TokenMap& generated, const char* key) {
      const auto it = generated.find(key);
      if (it == generated.end()) {
        return std::nullopt;
      }
      return toUi(Color::fromArgb(it->second));
    }

  } // namespace

  ::Color accentTextOn(const ::Color& accentFill, const ::Color& plane) {
    return toUi(ensureContrast(toInt(accentFill), toInt(plane), kMinContrast));
  }

  ::Color onAccentOn(const ::Color& preferred, const ::Color& accentFill) {
    return toUi(ensureContrast(toInt(preferred), toInt(accentFill), kMinContrast));
  }

  ::Palette recolorShellPalette(const ::Palette& tokens, const GeneratedPalette& palette, bool light) {
    const TokenMap& preferred = light ? palette.light : palette.dark;
    const TokenMap& generated = preferred.empty() ? (light ? palette.dark : palette.light) : preferred;
    const auto primary = role(generated, "primary");
    const auto surface = role(generated, "surface");
    if (!primary || !surface) {
      return tokens;
    }

    ::Palette out = tokens;
    out.accent = *primary;
    out.onAccent = tokens.onAccent;
    if (const auto on = role(generated, "on_primary")) {
      out.onAccent = *on;
    }
    out.onAccent = onAccentOn(out.onAccent, out.accent);
    out.accentText = accentTextOn(out.accent, *surface);

    // One plane colour: thin and thick materials would otherwise pull labels toward two different backgrounds.
    out.solidRegular = out.solidThin = out.tintRegular = out.tintThick = opaque(*surface);

    const std::array<::Color, 1> planes = {out.solidRegular};
    out.label1 = clampLabel(tokens.label1, planes);
    out.label2 = clampLabel(tokens.label2, planes);
    return out;
  }

} // namespace noctalia::theme
