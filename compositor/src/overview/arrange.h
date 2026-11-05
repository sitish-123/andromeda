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
  // True when no more than 2 px of `rect` lies on an output `outputWidth` wide (a scrolled-away column).
  [[nodiscard]] bool offscreen(const Rect& rect, double outputWidth);

  struct ExposeItem {
    uint32_t id = 0;
    Rect rect;
  };

  struct ExposeLimits {
    double padX = 40;      // horizontal room per cell around the window
    double padY = 56;      // vertical room per cell, title capsule included
    double caption = 28;   // the capsule's share of padY, kept under the window
    double maxScale = 0.7; // never shows a window larger than this share of its real size
  };

  // Exposé: every window scaled into a balanced grid inside `area` without overlap, rows filled in reading order of
  // the windows' real positions (rows of 200 px, then x), so a window stays roughly where it was. Fewer columns win
  // unless more columns show at least 2 % more window area. Returns one rect per item, in the order of `items`.
  [[nodiscard]] std::vector<ExposeItem>
  exposeGrid(std::span<const ExposeItem> items, const Rect& area, const ExposeLimits& limits = {});

} // namespace andromeda::ov
