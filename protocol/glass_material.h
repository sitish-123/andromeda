#pragma once

// The glass slider mapping (CLAUDE.md Decisions "Glass slider", "One glass system"), header-only so the shell
// (through libdesk-ui Style::glassMaterial) and the compositor use the same values. No other code maps the slider.

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace glass {

  // Shape: small liquid shapes (bar islands, dock, center island, launcher pill). Sheet: panels, sheets, cards,
  // windows. Menu: menus and popovers. Tooltip: tooltips (denser tint for small text).
  enum class Kind : std::uint8_t { Shape, Sheet, Menu, Tooltip };

  struct Material {
    // Tint alpha over the blur at the kind's reference opacity; 1 = solid (Reduce Transparency).
    float tint = 1.0F;
    // dsk_surface_v1 liquid [0]: rim refraction strength; blur strength follows it.
    float level = 0.0F;
    // dsk_surface_v1 liquid [1]: the specular rim.
    float rim = 0.0F;
    // dsk_surface_v1 liquid [2] (v9): the sharp backdrop through the rim band; costs a backdrop copy.
    float lens = 0.0F;
    // Blur size, 0 to 1.
    float blurStrength = 1.0F;

    bool operator==(const Material&) const = default;
  };

  // Low-contrast backdrops raise the tint halfway to this alpha, so the slider still shows; the colour never changes.
  inline constexpr float kDenseTint = 0.84F;

  // Clear blurs at 30 %, Tinted at 65 %, so the rim refraction bends shapes, not a smear.
  [[nodiscard]] constexpr float blurStrength(float level) noexcept { return 1.0F - 0.7F * level; }

  // Opacity a configured `opacity` of `kind` is relative to: tint_regular (0.70) for shapes, tint_thick otherwise.
  [[nodiscard]] constexpr float referenceOpacity(Kind kind, bool light) noexcept {
    return kind == Kind::Shape ? 0.70F : light ? 0.88F : 0.92F;
  }

  // slider: 0 Clear, 0.5 Tinted (the design values), 1 Frosted.
  [[nodiscard]] inline Material material(Kind kind, float slider, bool reduceTransparency, bool light) noexcept {
    if (reduceTransparency) {
      return {};
    }
    const float g = std::clamp(slider, 0.0F, 1.0F);
    const auto curve = [g](float clear, float tinted, float frosted) {
      return g < 0.5F ? std::lerp(clear, tinted, g * 2.0F) : std::lerp(tinted, frosted, g * 2.0F - 1.0F);
    };
    float tint = 0.0F;
    switch (kind) {
    case Kind::Shape:
      tint = light ? curve(0.20F, 0.34F, 0.84F) : curve(0.24F, 0.40F, 0.60F);
      break;
    case Kind::Sheet:
    case Kind::Menu:
      tint = light ? curve(0.20F, 0.34F, 0.95F) : curve(0.46F, 0.66F, 0.99F);
      break;
    case Kind::Tooltip:
      tint = light ? curve(0.70F, 0.84F, 0.95F) : curve(0.80F, 0.88F, 0.99F);
      break;
    }
    // Sheets blur a step denser than shapes: half the shapes' clarity.
    const float level = (kind == Kind::Shape ? 1.0F : 0.5F) * (1.0F - g);
    return {.tint = tint, .level = level, .rim = 1.0F, .lens = level, .blurStrength = blurStrength(level)};
  }

  // `t` of the way from a to b (a sheet that morphs from a shape).
  [[nodiscard]] inline Material mix(const Material& a, const Material& b, float t) noexcept {
    t = std::clamp(t, 0.0F, 1.0F);
    const float level = std::lerp(a.level, b.level, t);
    return {
        .tint = std::lerp(a.tint, b.tint, t), .level = level, .rim = std::lerp(a.rim, b.rim, t),
        .lens = std::lerp(a.lens, b.lens, t), .blurStrength = blurStrength(level)
    };
  }

  // The tint for a configured opacity (relative to referenceOpacity); an opacity of 1 is solid.
  [[nodiscard]] inline float tintFor(const Material& m, Kind kind, bool light, float opacity) noexcept {
    return opacity >= 1.0F || m.tint >= 1.0F ? 1.0F : std::min(1.0F, m.tint * opacity / referenceOpacity(kind, light));
  }

  // An opacity relative to tint_regular (0.70) rescaled to tint_thick (0.92), for sheets configured in regular terms.
  [[nodiscard]] constexpr float thickOpacity(float opacity) noexcept {
    return opacity >= 1.0F ? 1.0F : std::min(1.0F, opacity * (0.92F / 0.70F));
  }

  // The tint for a backdrop of the tint's own tone, `density` (0 to 1) of the way to the dense tint.
  [[nodiscard]] inline float denser(float tint, float density) noexcept {
    return tint + 0.5F * (std::max(tint, kDenseTint) - tint) * std::clamp(density, 0.0F, 1.0F);
  }

} // namespace glass
