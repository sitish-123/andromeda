#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

// Banner swipe (rulebook P9, §7.7). The first 8 px of travel claim an axis; a claimed axis stays until release.
// Up and right dismiss: the banner leaves, the notification stays in the notification center.
enum class BannerSwipeAxis : std::uint8_t { Undecided, Vertical, Horizontal };

inline constexpr float kBannerSwipeSlop = 8.0F;

[[nodiscard]] constexpr BannerSwipeAxis bannerSwipeAxis(BannerSwipeAxis axis, float dx, float dy) noexcept {
  if (axis != BannerSwipeAxis::Undecided || std::max(std::abs(dx), std::abs(dy)) <= kBannerSwipeSlop) {
    return axis;
  }
  return std::abs(dx) > std::abs(dy) ? BannerSwipeAxis::Horizontal : BannerSwipeAxis::Vertical;
}

// Offset along the claimed axis for `travel` (px since the press): 1:1 toward `limit` (the signed dismissal end, which
// it never passes), 0.3 the other way.
[[nodiscard]] constexpr float bannerSwipeOffset(float travel, float limit) noexcept {
  const bool toward = (limit < 0.0F) == (travel < 0.0F);
  const float offset = toward ? travel : travel * 0.3F;
  return limit < 0.0F ? std::max(offset, limit) : std::min(offset, limit);
}

// Release: the end projected with deceleration 0.998 (velocity in px/ms) snaps to the nearer of rest and `limit`; a flick
// faster than 300 px/s toward `limit` dismisses too.
[[nodiscard]] constexpr bool bannerSwipeDismisses(float offset, float velocity, float limit) noexcept {
  const float direction = limit < 0.0F ? -1.0F : 1.0F;
  const float projected = offset + velocity * 0.998F / (1.0F - 0.998F);
  return projected * direction > std::abs(limit) * 0.5F || velocity * direction > 0.3F;
}
