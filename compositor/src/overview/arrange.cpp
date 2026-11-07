#include "overview/arrange.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace andromeda::ov {

  Fit fitIn(const Rect& content, const Rect& area) {
    const double cw = std::max(content.w, 1.0);
    const double ch = std::max(content.h, 1.0);
    const double s = std::min(area.w / cw, area.h / ch);
    return {s, area.x + (area.w - cw * s) / 2 - content.x * s, area.y + (area.h - ch * s) / 2 - content.y * s};
  }

  Rect mapRect(const Rect& rect, const Fit& fit) {
    return {rect.x * fit.s + fit.ox, rect.y * fit.s + fit.oy, rect.w * fit.s, rect.h * fit.s};
  }

  Rect lerpRect(const Rect& from, const Rect& to, double t) {
    const auto mix = [t](double a, double b) { return a + (b - a) * t; };
    return {mix(from.x, to.x), mix(from.y, to.y), mix(from.w, to.w), mix(from.h, to.h)};
  }

  bool offscreen(const Rect& rect, double outputWidth) { return rect.x + rect.w <= 2 || rect.x >= outputWidth - 2; }

  std::vector<ExposeItem> exposeGrid(std::span<const ExposeItem> items, const Rect& area, const ExposeLimits& limits) {
    const size_t n = items.size();
    std::vector<ExposeItem> out(items.begin(), items.end());
    if (n == 0) {
      return out;
    }
    const auto size = [](const Rect& r) { return std::pair{std::max(r.w, 1.0), std::max(r.h, 1.0)}; };

    size_t bestCols = 1;
    double bestArea = -1;
    std::vector<double> bestScale;
    for (size_t cols = 1; cols <= n; ++cols) {
      const size_t rows = (n + cols - 1) / cols;
      const double cw = area.w / static_cast<double>(cols);
      const double ch = area.h / static_cast<double>(rows);
      double total = 0;
      std::vector<double> scale(n);
      for (size_t i = 0; i < n; ++i) {
        const auto [w, h] = size(items[i].rect);
        scale[i] = std::max(0.0, std::min({(cw - limits.padX) / w, (ch - limits.padY) / h, limits.maxScale}));
        total += scale[i] * scale[i] * w * h;
      }
      if (bestArea < 0 || total > bestArea * 1.02) {
        bestCols = cols;
        bestArea = total;
        bestScale = std::move(scale);
      }
    }

    const size_t cols = bestCols;
    const size_t rows = (n + cols - 1) / cols;
    const double cw = area.w / static_cast<double>(cols);
    const double ch = area.h / static_cast<double>(rows);
    std::vector<size_t> order(n);
    std::ranges::iota(order, size_t{0});
    std::ranges::stable_sort(order, [&](size_t a, size_t b) {
      const Rect& ra = items[a].rect;
      const Rect& rb = items[b].rect;
      const double rowA = std::round(ra.y / 200);
      const double rowB = std::round(rb.y / 200);
      return rowA != rowB ? rowA < rowB : ra.x < rb.x;
    });
    for (size_t k = 0; k < n; ++k) {
      const size_t i = order[k];
      const size_t row = k / cols;
      const size_t col = k % cols;
      const size_t inRow = std::min(cols, n - row * cols);
      const double x0 = area.x + (area.w - static_cast<double>(inRow) * cw) / 2 + static_cast<double>(col) * cw;
      const double y0 = area.y + (area.h - static_cast<double>(rows) * ch) / 2 + static_cast<double>(row) * ch;
      const auto [w, h] = size(items[i].rect);
      const double s = bestScale[i];
      out[i].rect = {x0 + (cw - w * s) / 2, y0 + (ch - limits.caption - h * s) / 2, w * s, h * s};
    }
    return out;
  }

  PixelBox snapEdges(const Rect& rect) {
    const auto x0 = static_cast<int>(std::lround(rect.x));
    const auto y0 = static_cast<int>(std::lround(rect.y));
    const auto x1 = static_cast<int>(std::lround(rect.x + rect.w));
    const auto y1 = static_cast<int>(std::lround(rect.y + rect.h));
    return {x0, y0, std::max(1, x1 - x0), std::max(1, y1 - y0)};
  }

} // namespace andromeda::ov
