#pragma once

#include <algorithm>
#include <cmath>

// The live island in a left or right bar lane (DESIGN.md "Vertical-bar live island"), in layout px.
namespace island_lane {

  inline constexpr float kCardGap = 8.0F;   // pill to the expanded card
  inline constexpr float kSymbolPad = 8.0F; // pill end next to a glyph, a dot or the art
  inline constexpr float kTextPad = 12.0F;  // pill end next to text

  struct Box {
    float x = 0.0F;
    float y = 0.0F;
    float w = 0.0F;
    float h = 0.0F;
  };

  // The pill is as wide as the bar: `barThickness` in logical px, `scale` the bar's content scale.
  [[nodiscard]] constexpr float pillWidth(float barThickness, float scale, float cap) noexcept {
    return std::min(barThickness / scale, cap);
  }

  struct Length {
    float height = 0.0F;
    float padTop = 0.0F;
  };

  // The pill hugs its stacked content: 12 at a text end, 8 at a symbol end.
  [[nodiscard]] constexpr Length pillLength(float content, bool textFirst, bool textLast) noexcept {
    const float top = textFirst ? kTextPad : kSymbolPad;
    return {.height = top + content + (textLast ? kTextPad : kSymbolPad), .padTop = top};
  }

  // `pad`: surface columns between the screen-side surface edge and the pill. The surface is centred on the output.
  [[nodiscard]] inline Box pill(float surfaceW, float surfaceH, float pad, float w, float h, bool right) noexcept {
    return {.x = right ? surfaceW - pad - w : pad, .y = std::round((surfaceH - h) * 0.5F), .w = w, .h = h};
  }

  // An expanded face grows sideways: 8 from the pill, top edges equal, inside the surface.
  [[nodiscard]] inline Box card(const Box& pill, float surfaceH, float w, float h, bool right) noexcept {
    return {
        .x = right ? pill.x - kCardGap - w : pill.x + pill.w + kCardGap,
        .y = std::clamp(pill.y, 0.0F, std::max(0.0F, surfaceH - h)),
        .w = w,
        .h = h,
    };
  }

  // The gap between the pill and its card, on the pill's rows: input there keeps the hover while the pointer crosses.
  [[nodiscard]] constexpr Box gap(const Box& pill, bool right) noexcept {
    return {.x = right ? pill.x - kCardGap : pill.x + pill.w, .y = pill.y, .w = kCardGap, .h = pill.h};
  }

  [[nodiscard]] constexpr float surfaceWidth(float pad, float pillW, float cardW) noexcept {
    return pad + pillW + kCardGap + cardW;
  }

} // namespace island_lane
