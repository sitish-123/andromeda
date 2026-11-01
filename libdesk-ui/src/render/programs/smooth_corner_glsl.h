#pragma once

#include "design_tokens.h"

#include <format>
#include <string>
#include <string_view>

// Continuous-curvature corners (radius.smoothing in protocol/design-tokens.toml). The compositor's corner_alpha.frag
// uses the same formula, so client shapes, compositor clips and blur masks line up.
namespace smooth_corner {

  // Signed distance (approximate off the edge; the shaders normalize coverage by its screen-space rate) to a box of
  // half size half_size whose corner has radius r. The corner is a superellipse starting r * (1 + 0.88 s) from the
  // corner with exponent 2 + 5 s; s shrinks to 0 when the box is too small to grow the corner (pills, circles).
  inline constexpr std::string_view kGlsl = R"(
float smooth_rounded_box(vec2 centered, vec2 half_size, float r) {
    float max_r = min(half_size.x, half_size.y);
    float s = r > 0.0 ? clamp((max_r / r - 1.0) / 0.88, 0.0, CORNER_SMOOTHING) : 0.0;
    float e = min(r * (1.0 + 0.88 * s), max_r);
    vec2 q = abs(centered) - (half_size - vec2(e));
    if (s > 0.001 && q.x > 0.0 && q.y > 0.0) {
        float n = 2.0 + 5.0 * s;
        vec2 u = q / e;
        return (pow(pow(u.x, n) + pow(u.y, n), 1.0 / n) - 1.0) * e;
    }
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - e;
}
)";

  // fragmentSource with CORNER_SMOOTHING defined and smooth_rounded_box() in place of the "// @smooth_corner" line.
  inline std::string fragment(std::string_view fragmentSource) {
    constexpr std::string_view kMarker = "// @smooth_corner";
    std::string src = std::format("#define CORNER_SMOOTHING {:.4f}\n", Tokens::radiusSmoothing);
    const auto at = fragmentSource.find(kMarker);
    if (at == std::string_view::npos) {
      src += fragmentSource;
      return src;
    }
    src += fragmentSource.substr(0, at);
    src += kGlsl;
    src += fragmentSource.substr(at + kMarker.size());
    return src;
  }

} // namespace smooth_corner
