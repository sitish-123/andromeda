#pragma once

#include <cstdint>
#include <span>
#include <vector>

// Pure overview geometry: no scene, View or wlroots types, so every rule here is unit-tested in isolation
// (tests/unit/overview_arrange.cpp). The behaviour reference is .agent/mockups/overview-proto/geom.js.
namespace andromeda::ov {

  struct Rect {
    double x = 0;
    double y = 0;
    double w = 0;
    double h = 0;

    bool operator==(const Rect&) const = default;
  };

  // A uniform scale plus offset that maps one rect space into another.
  struct Fit {
    double s = 1;
    double ox = 0;
    double oy = 0;
  };

  // The largest uniform scale that puts `content` inside `area`, centred.
  [[nodiscard]] Fit fitIn(const Rect& content, const Rect& area);
  [[nodiscard]] Rect mapRect(const Rect& rect, const Fit& fit);
  [[nodiscard]] Rect lerpRect(const Rect& from, const Rect& to, double t);
  struct PixelBox {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;

    bool operator==(const PixelBox&) const = default;
  };

  // Rounds each edge once (x1 = round(x + w), width = x1 - x0), never the size on its own: a rect that moves or scales
  // smoothly then moves every edge one way only, where rounding x and w apart steps an edge back by 1 px now and then.
  [[nodiscard]] PixelBox snapEdges(const Rect& rect);

  // True when no more than 2 px of `rect` lies on an output `outputWidth` wide (a scrolled-away column).
  [[nodiscard]] bool offscreen(const Rect& rect, double outputWidth);

  struct ExposeItem {
    uint32_t id = 0;
    Rect rect;
  };

  struct ExposeLimits {
    double padX = 40;      // horizontal room around each window
    double padY = 56;      // vertical room per row, title capsule included
    double caption = 28;   // the capsule's share of padY, kept under the window
    double maxScale = 0.7; // never shows a window larger than this share of its real size
  };

  // Exposé: every window at one shared scale inside `area` without overlap, in rows filled in reading order of the
  // windows' real positions (rows of 200 px, then x), so a window stays roughly where it was and keeps its size next
  // to the others. Each row is as wide as its windows and centred. Fewer columns win unless more columns show at least
  // 2 % more window area. Returns one rect per item, in the order of `items`.
  [[nodiscard]] std::vector<ExposeItem>
  exposeGrid(std::span<const ExposeItem> items, const Rect& area, const ExposeLimits& limits = {});

} // namespace andromeda::ov
