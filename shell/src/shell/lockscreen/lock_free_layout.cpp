#include "shell/lockscreen/lock_free_layout.h"

#include <algorithm>
#include <cmath>

namespace lock_free {

  namespace {

    // Status island: top 8, h 32, plus 16.
    constexpr float kTopEdge = 56.0F;
    constexpr float kGap = 24.0F;
    // Account column: avatar 64, g 12, name 20, g 16, capsule 40, g 12, hint 16.
    constexpr float kAccountHead = 64.0F + 12.0F + 20.0F + 16.0F;
    constexpr float kAccountFoot = 40.0F + 12.0F + 16.0F;
    constexpr float kAuthHalfWidth = 180.0F;
    // Mock section 2 ("Snap to zones"): bands sit 80 in from the sides, 24 apart.
    constexpr float kSide = 80.0F;

    AuthRect span(float x, float y, float right, float bottom) {
      return {x, y, std::max(0.0F, right - x), std::max(0.0F, bottom - y)};
    }

    float snapTo(float v, float grid) { return std::round(v / grid) * grid; }

    bool contains(const AuthRect& r, float x, float y) { return x >= r.x && x < r.right() && y >= r.y && y < r.bottom(); }

  } // namespace

  Zones zones(const AuthPlacement& p, float sw, float sh, float scale) {
    const float s = std::max(0.1F, scale);
    Zones z;
    const float left = kSide * s;
    const float right = std::max(left, sw - kSide * s);
    const float half = kGap * 0.5F * s;
    const float headTop = p.capsule.y - (p.account ? kAccountHead : 0.0F) * s;
    // Centered and Minimal put the auth column under the content: Auth is the band from the column down (mock
    // section 2). Split and Glance keep the column to the side or centred: Auth is the column with a 24 margin, and
    // the middle band runs to the bottom.
    const bool below = p.layout == AuthLayout::Centered || p.layout == AuthLayout::Minimal;
    z.auth = below ? span(left, headTop - kGap * s, right, sh - kGap * s)
                   : span(
                         p.columnCx - (kAuthHalfWidth + kGap) * s, headTop - kGap * s,
                         p.columnCx + (kAuthHalfWidth + kGap) * s, p.capsule.y + (kAccountFoot + kGap) * s
                     );
    const float clockBottom = p.clockY + p.clockHeight();
    const float split = std::clamp(clockBottom + kGap * s, kTopEdge * s, sh);
    z.top = span(left, kTopEdge * s, right, split - half);
    z.middle = span(left, split + half, right, below ? std::max(split + half, z.auth.y - kGap * s) : sh - kTopEdge * s);
    return z;
  }

  Zone zoneOf(const AuthRect& box, const Zones& z) {
    const float cx = box.x + box.w * 0.5F;
    const float cy = box.y + box.h * 0.5F;
    if (contains(z.top, cx, cy)) {
      return Zone::Top;
    }
    if (contains(z.middle, cx, cy)) {
      return Zone::Middle;
    }
    return Zone::None;
  }

  bool overlaps(const AuthRect& a, const AuthRect& b) {
    return a.x < b.right() && b.x < a.right() && a.y < b.bottom() && b.y < a.bottom();
  }

  std::optional<AuthRect> place(
      AuthRect box, const Zones& z, bool snapZones, Zone zone, bool snap, float sw, float sh, float scale
  ) {
    const float s = std::max(0.1F, scale);
    if (snap) {
      const float grid = (snapZones && zone != Zone::None ? kZoneGrid : kFreeGrid) * s;
      box.x = snapTo(box.x, grid);
      box.y = snapTo(box.y, grid);
    }
    AuthRect bounds{0.0F, 0.0F, sw, sh};
    if (snapZones && zone != Zone::None) {
      bounds = zone == Zone::Top ? z.top : z.middle;
    }
    // A box larger than its bounds keeps its size and sits at the top left of them.
    box.x = std::clamp(box.x, bounds.x, std::max(bounds.x, bounds.right() - box.w));
    box.y = std::clamp(box.y, bounds.y, std::max(bounds.y, bounds.bottom() - box.h));
    if (overlaps(box, z.auth)) {
      return std::nullopt;
    }
    return box;
  }

  AuthRect nudge(AuthRect box, int dx, int dy, bool big, bool resize, float scale) {
    const float s = std::max(0.1F, scale);
    const float step = (big ? kFreeGrid : kZoneGrid) * s;
    if (resize) {
      box.w = std::max(kMinSize * s, box.w + static_cast<float>(dx) * step);
      box.h = std::max(kMinSize * s, box.h + static_cast<float>(dy) * step);
    } else {
      box.x += static_cast<float>(dx) * step;
      box.y += static_cast<float>(dy) * step;
    }
    return box;
  }

} // namespace lock_free
