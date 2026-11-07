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
    std::vector<size_t> order(n);
    std::ranges::iota(order, size_t{0});
    std::ranges::stable_sort(order, [&](size_t a, size_t b) {
      const Rect& ra = items[a].rect;
      const Rect& rb = items[b].rect;
      const double rowA = std::round(ra.y / 200);
      const double rowB = std::round(rb.y / 200);
      return rowA != rowB ? rowA < rowB : ra.x < rb.x;
    });

    // One scale for every window, so they keep their relative sizes; each row is as wide as its windows.
    const auto scaleFor = [&](size_t cols) {
      const size_t rows = (n + cols - 1) / cols;
      double height = 0;
      double s = limits.maxScale;
      for (size_t row = 0; row < rows; ++row) {
        double width = 0;
        double tallest = 0;
        const size_t end = std::min(n, (row + 1) * cols);
        for (size_t k = row * cols; k < end; ++k) {
          const auto [w, h] = size(items[order[k]].rect);
          width += w;
          tallest = std::max(tallest, h);
        }
        s = std::min(s, (area.w - limits.padX * static_cast<double>(end - row * cols)) / width);
        height += tallest;
      }
      return std::max(0.0, std::min(s, (area.h - limits.padY * static_cast<double>(rows)) / height));
    };
    size_t cols = 1;
    double best = scaleFor(1);
    for (size_t c = 2; c <= n; ++c) {
      // Window area grows with the square of the scale; fewer columns win unless more show at least 2 % more.
      const double s = scaleFor(c);
      if (s * s > best * best * 1.02) {
        cols = c;
        best = s;
      }
    }

    const size_t rows = (n + cols - 1) / cols;
    std::vector<double> rowHeight(rows, 0.0);
    for (size_t k = 0; k < n; ++k) {
      rowHeight[k / cols] = std::max(rowHeight[k / cols], size(items[order[k]].rect).second * best + limits.padY);
    }
    double y = area.y + (area.h - std::accumulate(rowHeight.begin(), rowHeight.end(), 0.0)) / 2;
    for (size_t row = 0; row < rows; ++row) {
      const size_t begin = row * cols;
      const size_t end = std::min(n, begin + cols);
      double width = 0;
      for (size_t k = begin; k < end; ++k) {
        width += size(items[order[k]].rect).first * best + limits.padX;
      }
      double x = area.x + (area.w - width) / 2;
      for (size_t k = begin; k < end; ++k) {
        const size_t i = order[k];
        const auto [w, h] = size(items[i].rect);
        const double cellW = w * best + limits.padX;
        out[i].rect = {x + limits.padX / 2, y + (rowHeight[row] - limits.caption - h * best) / 2, w * best, h * best};
        x += cellW;
      }
      y += rowHeight[row];
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
