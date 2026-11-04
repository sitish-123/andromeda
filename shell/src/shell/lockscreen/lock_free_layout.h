#pragma once

#include "ui/auth/auth_layout.h"

#include <optional>

// Free mode of the lock screen editor (mock section 2): where a widget box may go. Logical px; scale = ui_scale.
namespace lock_free {

  // Free snaps to 32; inside a zone ("Snap to zones") to the 4 grid.
  constexpr float kFreeGrid = 32.0F;
  constexpr float kZoneGrid = 4.0F;
  constexpr float kMinSize = 56.0F;

  enum class Zone { None, Top, Middle };

  struct Zones {
    // Top: below the status island to 24 under the clock block. Middle: from there to 24 above the auth area.
    AuthRect top;
    AuthRect middle;
    // The account column with a 24 margin; never covered.
    AuthRect auth;
  };

  [[nodiscard]] Zones zones(const AuthPlacement& place, float sw, float sh, float scale);
  // The band that holds the box centre; None outside both.
  [[nodiscard]] Zone zoneOf(const AuthRect& box, const Zones& zones);
  [[nodiscard]] bool overlaps(const AuthRect& a, const AuthRect& b);

  // A dropped (snap = true) or nudged box: snapped to the grid, kept on the output, and with snapZones kept inside
  // `zone`. nullopt = the box would cover the auth area (the caller keeps the old box).
  [[nodiscard]] std::optional<AuthRect> place(
      AuthRect box, const Zones& zones, bool snapZones, Zone zone, bool snap, float sw, float sh, float scale
  );

  // Arrow keys: move by 4 (big: 32), or with resize grow / shrink the right and bottom edge (min kMinSize).
  [[nodiscard]] AuthRect nudge(AuthRect box, int dx, int dy, bool big, bool resize, float scale);

} // namespace lock_free
