#pragma once

#include <algorithm>
#include <cmath>

// Ambient light (lux) to a display brightness level (0..1), shifted by the offset the user taught it.
// The curve is logarithmic: 0 lux gives 0.1, 10^4 lux (daylight) gives 1.
class AmbientBrightnessModel {
public:
  [[nodiscard]] static float curve(double lux) {
    const double t = std::log10(1.0 + std::max(0.0, lux)) / 4.0;
    return static_cast<float>(std::clamp(0.1 + (0.9 * t), 0.0, 1.0));
  }
  [[nodiscard]] float target(double lux) const { return std::clamp(curve(lux) + m_offset, 0.01F, 1.0F); }
  // The user chose `level` at `lux`: later targets keep that distance from the curve.
  void learn(double lux, float level) { m_offset = std::clamp(level - curve(lux), -0.5F, 0.5F); }

private:
  float m_offset = 0.0F;
};
