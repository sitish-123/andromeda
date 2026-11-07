#include "scene/ssd_frame_geometry.h"

#include <algorithm>
#include <ranges>
#include <vector>

extern "C" {
#include <wlr/util/edges.h>
}

namespace andromeda {
  int titleBarButtonDiameter(int height) {
    const int diameter = std::max(8, (height + 1) / 2);
    if ((height - diameter) % 2 == 0)
      return diameter;
    return diameter > 8 ? diameter - 1 : diameter + 1;
  }

  TitleBarLayout titleBarLayout(
      int width, int height, TitleBarButtonSide side, std::span<const TitleBarButton> shown, TitleBarStyle style
  ) {
    // Windows: line glyphs centred in 46 wide full-height cells. macOS: 12 px traffic lights 8 apart, 12 from the end.
    const bool cells = style == TitleBarStyle::Windows;
    const bool mac = style == TitleBarStyle::Macos;
    const int diameter = cells ? std::min(10, height / 2) : mac ? std::min(12, height) : titleBarButtonDiameter(height);
    const int inset = (height - diameter) / 2;
    const int edge = mac ? 12 : inset;
    const int cellWidth = cells ? 46 : diameter + kTitleBarButtonGap;
    const int pitch = cellWidth;
    TitleBarLayout result{.title = {kTitleBarTitlePadding, 0, 0, height}, .centredTitle = style == TitleBarStyle::Macos};
    // Walk from the edge inward: the last listed button is outermost. `x` is the cell's left edge.
    int x = side == TitleBarButtonSide::Right ? width - (cells ? cellWidth : edge + diameter + kTitleBarButtonGap / 2)
                                              : (cells ? 0 : edge - kTitleBarButtonGap / 2);
    const int step = side == TitleBarButtonSide::Right ? -pitch : pitch;
    std::optional<int> innermost; // the inner edge of the innermost cell
    // macOS keeps its own order from the edge: close, minimize, zoom.
    std::vector<TitleBarButton> order(shown.begin(), shown.end());
    if (mac) {
      std::vector<TitleBarButton> lights;
      for (const TitleBarButton button : {TitleBarButton::Maximize, TitleBarButton::Minimize, TitleBarButton::Close}) {
        if (std::ranges::contains(shown, button)) {
          lights.push_back(button);
        }
      }
      order = std::move(lights);
    }
    for (const TitleBarButton button : std::views::reverse(order)) {
      const auto index = static_cast<size_t>(button);
      if (result.buttons[index]) {
        continue;
      }
      result.cells[index] = wlr_box{x, 0, cellWidth, height};
      result.buttons[index] = wlr_box{x + (cellWidth - diameter) / 2, inset, diameter, diameter};
      innermost = side == TitleBarButtonSide::Right ? x : x + cellWidth;
      x += step;
    }
    const int edgeGap = cells ? kTitleBarTitlePadding : kTitleBarTitlePadding - kTitleBarButtonGap / 2;
    if (side == TitleBarButtonSide::Right) {
      result.title.width = std::max(0, innermost.value_or(width + edgeGap - kTitleBarTitlePadding) - edgeGap
                                           - kTitleBarTitlePadding);
    } else {
      if (innermost) {
        result.title.x = *innermost + edgeGap;
      }
      result.title.width = std::max(0, width - result.title.x - kTitleBarTitlePadding);
    }
    if (result.centredTitle) {
      // Equal room on both sides, so the title centres on the bar, not on the space beside the buttons.
      const int reserve = std::max(result.title.x, width - result.title.x - result.title.width);
      result.title = {reserve, 0, std::max(0, width - 2 * reserve), height};
    }
    return result;
  }

  SsdFramePoint ssdFramePointAt(
      int cw, int ch, int h, int b, bool resizable, TitleBarButtonSide side, std::span<const TitleBarButton> shown,
      double x, double y, TitleBarStyle style
  ) {
    if (cw <= 0 || ch <= 0 || h < 0)
      return {};
    if (x >= 0 && x < cw && y >= -h && y < 0) {
      const TitleBarLayout layout = titleBarLayout(cw, h, side, shown, style);
      const auto inButton = [&layout, x](TitleBarButton button) {
        const std::optional<wlr_box>& box = layout.cells[static_cast<size_t>(button)];
        return box && x >= box->x && x < box->x + box->width;
      };
      if (inButton(TitleBarButton::Close))
        return {SsdFramePart::Close, 0};
      if (inButton(TitleBarButton::Maximize))
        return {SsdFramePart::Maximize, 0};
      if (inButton(TitleBarButton::Minimize))
        return {SsdFramePart::Minimize, 0};
      return {SsdFramePart::TitleBar, 0};
    }
    if (!resizable
        || x < -b - kSsdResizeBand
        || x >= cw + b + kSsdResizeBand
        || y < -h - b - kSsdResizeBand
        || y >= ch + b + kSsdResizeBand)
      return {};
    uint32_t edges = 0;
    if (x < 0)
      edges |= WLR_EDGE_LEFT;
    if (x >= cw)
      edges |= WLR_EDGE_RIGHT;
    if (y < -h)
      edges |= WLR_EDGE_TOP;
    if (y >= ch)
      edges |= WLR_EDGE_BOTTOM;
    if (edges == 0)
      return {};
    if ((edges & (WLR_EDGE_LEFT | WLR_EDGE_RIGHT)) != 0) {
      if (y < -h - b + kSsdResizeCorner)
        edges |= WLR_EDGE_TOP;
      if (y >= ch + b - kSsdResizeCorner)
        edges |= WLR_EDGE_BOTTOM;
    }
    if ((edges & (WLR_EDGE_TOP | WLR_EDGE_BOTTOM)) != 0) {
      if (x < -b + kSsdResizeCorner)
        edges |= WLR_EDGE_LEFT;
      if (x >= cw + b - kSsdResizeCorner)
        edges |= WLR_EDGE_RIGHT;
    }
    return {SsdFramePart::Resize, edges};
  }

  wlr_box contentBoxInTile(const wlr_box& tile, int titleBarHeight) {
    return {tile.x, tile.y + titleBarHeight, tile.width, std::max(1, tile.height - titleBarHeight)};
  }
} // namespace andromeda
