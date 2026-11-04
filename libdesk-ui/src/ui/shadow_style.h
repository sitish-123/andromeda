#pragma once

#include <cstdint>

// Drop-shadow look of toolkit popups and surfaces. The shell config stores it as ShellConfig::ShadowConfig.
enum class ShadowDirection : std::uint8_t {
  Center = 0,
  Down = 1,
  Up = 2,
  Left = 3,
  Right = 4,
  DownLeft = 5,
  DownRight = 6,
  UpLeft = 7,
  UpRight = 8,
};

struct ShadowStyle {
  ShadowDirection direction = ShadowDirection::Down;
  float alpha = 0.55F;

  bool operator==(const ShadowStyle&) const = default;
};

struct ShadowDirectionOffset {
  std::int32_t x;
  std::int32_t y;
};

constexpr ShadowDirectionOffset shadowDirectionOffset(ShadowDirection dir) noexcept {
  switch (dir) {
  case ShadowDirection::Center:
    return {0, 0};
  case ShadowDirection::Down:
    return {0, 2};
  case ShadowDirection::Up:
    return {0, -2};
  case ShadowDirection::Left:
    return {-2, 0};
  case ShadowDirection::Right:
    return {2, 0};
  case ShadowDirection::DownLeft:
    return {-2, 2};
  case ShadowDirection::DownRight:
    return {2, 2};
  case ShadowDirection::UpLeft:
    return {-2, -2};
  case ShadowDirection::UpRight:
    return {2, -2};
  }
  return {0, 2};
}
