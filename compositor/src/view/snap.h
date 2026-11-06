#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

extern "C" {
#include <wlr/util/box.h>
}

namespace andromeda {
  enum class SnapRegion : uint8_t { Left, Right, Top, Bottom, TopLeft, TopRight, BottomLeft, BottomRight };
  constexpr int kSnapEdge = 16;
  constexpr int kSnapCornerSpan = 100;
  constexpr int kSnapDwellMs = 150;

  [[nodiscard]] std::string_view snapRegionName(SnapRegion region);
  [[nodiscard]] std::optional<SnapRegion> snapRegionNamed(std::string_view name);
  [[nodiscard]] wlr_box snapRegionBox(const wlr_box& area, SnapRegion region, int spacing);
  // Output coordinates and thresholds are logical. Shared edges are excluded until the drag dwell expires.
  [[nodiscard]] std::optional<SnapRegion> snapZoneAt(const wlr_box& output, double x, double y, uint32_t closedEdges);

  // Snap Layouts (Windows 11, prototype SNAP_LAYOUTS): six layouts of zones over the work area. Named "<layout>.<zone>",
  // both from 1: halves, 2/3 + 1/3, thirds, 1 + 2 stacked, quarters, 1/4 + 1/2 + 1/4.
  struct SnapZone {
    uint8_t layout = 0;
    uint8_t zone = 0;
    bool operator==(const SnapZone&) const = default;
  };
  constexpr size_t kSnapLayouts = 6;
  [[nodiscard]] size_t snapLayoutZones(size_t layout);
  // A zone as fractions of the work area: x, y, width, height.
  struct SnapFraction {
    double x, y, w, h;
  };
  [[nodiscard]] SnapFraction snapZoneFraction(SnapZone zone);
  // Zone edges inside the area keep half of `spacing` each, so neighbouring zones sit `spacing` apart.
  [[nodiscard]] wlr_box snapZoneBox(const wlr_box& area, SnapZone zone, int spacing);

  // Where a snapped floating window sits: a half or quarter from an edge drag, or a Snap Layouts zone.
  using SnapSpot = std::variant<SnapRegion, SnapZone>;
  [[nodiscard]] std::string snapSpotName(const SnapSpot& spot);
  [[nodiscard]] std::optional<SnapSpot> snapSpotNamed(std::string_view name);
  [[nodiscard]] wlr_box snapSpotBox(const wlr_box& area, const SnapSpot& spot, int spacing);
} // namespace andromeda
