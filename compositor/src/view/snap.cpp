#include "view/snap.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <format>
#include <utility>

extern "C" {
#include <wlr/util/edges.h>
}

namespace andromeda {
  namespace {
    constexpr std::array kNames{"left",     "right",     "top",         "bottom",
                                "top_left", "top_right", "bottom_left", "bottom_right"};
  }

  std::string_view snapRegionName(SnapRegion region) { return kNames[static_cast<size_t>(region)]; }

  std::optional<SnapRegion> snapRegionNamed(std::string_view name) {
    for (size_t i = 0; i < kNames.size(); ++i) {
      if (name == kNames[i]) {
        return static_cast<SnapRegion>(i);
      }
    }
    return std::nullopt;
  }

  wlr_box snapRegionBox(const wlr_box& area, SnapRegion region, int spacing) {
    const bool right =
        region == SnapRegion::Right || region == SnapRegion::TopRight || region == SnapRegion::BottomRight;
    const bool splitX = region != SnapRegion::Top && region != SnapRegion::Bottom;
    const bool splitY = region != SnapRegion::Left && region != SnapRegion::Right;
    const bool bottom =
        region == SnapRegion::Bottom || region == SnapRegion::BottomLeft || region == SnapRegion::BottomRight;
    const int gapX = std::clamp(spacing, 0, std::max(0, area.width));
    const int gapY = std::clamp(spacing, 0, std::max(0, area.height));
    const int leftWidth = std::max(0, area.width - gapX) / 2;
    const int topHeight = std::max(0, area.height - gapY) / 2;
    return {
        .x = area.x + (right ? leftWidth + gapX : 0),
        .y = area.y + (bottom ? topHeight + gapY : 0),
        .width = splitX ? (right ? std::max(0, area.width - gapX - leftWidth) : leftWidth) : area.width,
        .height = splitY ? (bottom ? std::max(0, area.height - gapY - topHeight) : topHeight) : area.height,
    };
  }

  std::optional<SnapRegion> snapZoneAt(const wlr_box& output, double x, double y, uint32_t closedEdges) {
    if (output.width <= 0
        || output.height <= 0
        || x < output.x
        || y < output.y
        || x >= output.x + output.width
        || y >= output.y + output.height) {
      return std::nullopt;
    }
    const double left = x - output.x;
    const double right = output.x + output.width - x;
    const double top = y - output.y;
    const double bottom = output.y + output.height - y;
    const bool onLeft = left < kSnapEdge && (closedEdges & WLR_EDGE_LEFT) == 0;
    const bool onRight = right <= kSnapEdge && (closedEdges & WLR_EDGE_RIGHT) == 0;
    const bool onTop = top < kSnapEdge && (closedEdges & WLR_EDGE_TOP) == 0;
    const bool onBottom = bottom <= kSnapEdge && (closedEdges & WLR_EDGE_BOTTOM) == 0;
    if ((onLeft && top < kSnapCornerSpan) || (onTop && left < kSnapCornerSpan))
      return SnapRegion::TopLeft;
    if ((onRight && top < kSnapCornerSpan) || (onTop && right <= kSnapCornerSpan))
      return SnapRegion::TopRight;
    if ((onLeft && bottom <= kSnapCornerSpan) || (onBottom && left < kSnapCornerSpan))
      return SnapRegion::BottomLeft;
    if ((onRight && bottom <= kSnapCornerSpan) || (onBottom && right <= kSnapCornerSpan))
      return SnapRegion::BottomRight;
    if (onTop)
      return SnapRegion::Top;
    if (onBottom)
      return SnapRegion::Bottom;
    if (onLeft)
      return SnapRegion::Left;
    if (onRight)
      return SnapRegion::Right;
    return std::nullopt;
  }

  namespace {
    constexpr std::array<std::array<SnapFraction, 4>, kSnapLayouts> kZones{{
        {{{0, 0, 0.5, 1}, {0.5, 0, 0.5, 1}}},
        {{{0, 0, 2.0 / 3, 1}, {2.0 / 3, 0, 1.0 / 3, 1}}},
        {{{0, 0, 1.0 / 3, 1}, {1.0 / 3, 0, 1.0 / 3, 1}, {2.0 / 3, 0, 1.0 / 3, 1}}},
        {{{0, 0, 0.5, 1}, {0.5, 0, 0.5, 0.5}, {0.5, 0.5, 0.5, 0.5}}},
        {{{0, 0, 0.5, 0.5}, {0.5, 0, 0.5, 0.5}, {0, 0.5, 0.5, 0.5}, {0.5, 0.5, 0.5, 0.5}}},
        {{{0, 0, 0.25, 1}, {0.25, 0, 0.5, 1}, {0.75, 0, 0.25, 1}}},
    }};
    constexpr std::array<size_t, kSnapLayouts> kZoneCounts{2, 2, 3, 3, 4, 3};
  } // namespace

  size_t snapLayoutZones(size_t layout) { return layout < kSnapLayouts ? kZoneCounts[layout] : 0; }

  SnapFraction snapZoneFraction(SnapZone zone) {
    return zone.zone < snapLayoutZones(zone.layout) ? kZones[zone.layout][zone.zone] : SnapFraction{0, 0, 1, 1};
  }

  wlr_box snapZoneBox(const wlr_box& area, SnapZone zone, int spacing) {
    const SnapFraction f = snapZoneFraction(zone);
    const double half = std::max(0, spacing) / 2.0;
    const auto edge = [](int origin, int size, double fraction) { return origin + size * fraction; };
    double x0 = edge(area.x, area.width, f.x);
    double x1 = edge(area.x, area.width, f.x + f.w);
    double y0 = edge(area.y, area.height, f.y);
    double y1 = edge(area.y, area.height, f.y + f.h);
    x0 += f.x > 0 ? half : 0;
    x1 -= f.x + f.w < 0.999 ? half : 0;
    y0 += f.y > 0 ? half : 0;
    y1 -= f.y + f.h < 0.999 ? half : 0;
    const int left = static_cast<int>(std::lround(x0));
    const int top = static_cast<int>(std::lround(y0));
    return {
        .x = left,
        .y = top,
        .width = std::max(0, static_cast<int>(std::lround(x1)) - left),
        .height = std::max(0, static_cast<int>(std::lround(y1)) - top),
    };
  }

  std::string snapSpotName(const SnapSpot& spot) {
    if (const auto* zone = std::get_if<SnapZone>(&spot)) {
      return std::format("{}.{}", zone->layout + 1, zone->zone + 1);
    }
    return std::string(snapRegionName(std::get<SnapRegion>(spot)));
  }

  std::optional<SnapSpot> snapSpotNamed(std::string_view name) {
    if (const auto region = snapRegionNamed(name)) {
      return SnapSpot{*region};
    }
    const size_t dot = name.find('.');
    unsigned layout = 0;
    unsigned zone = 0;
    const auto number = [](std::string_view text, unsigned& out) {
      const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), out);
      return error == std::errc{} && end == text.data() + text.size();
    };
    if (dot == std::string_view::npos || !number(name.substr(0, dot), layout) || !number(name.substr(dot + 1), zone)
        || layout < 1 || zone < 1 || zone > snapLayoutZones(layout - 1)) {
      return std::nullopt;
    }
    return SnapSpot{SnapZone{.layout = static_cast<uint8_t>(layout - 1), .zone = static_cast<uint8_t>(zone - 1)}};
  }

  wlr_box snapSpotBox(const wlr_box& area, const SnapSpot& spot, int spacing) {
    if (const auto* zone = std::get_if<SnapZone>(&spot)) {
      return snapZoneBox(area, *zone, spacing);
    }
    return snapRegionBox(area, std::get<SnapRegion>(spot), spacing);
  }
} // namespace andromeda
