#include "overview/style.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>

namespace andromeda::ov {

  namespace {

    constexpr std::array<std::string_view, 4> kStyleNames = {"spaces", "taskview", "strip", "grid"};

    // Spaces + Task View: thumbs in one row, sized down when they would not fit between the margins.
    constexpr double kThumbGap = 18;
    constexpr double kSpacesThumbWidth = 176;
    constexpr double kTaskViewThumbWidth = 208;
    constexpr double kThumbTop = 56;     // Spaces: thumb row from the top edge, below the 32 px center island at 8
    constexpr double kPlusShare = 0.64;  // Spaces: the + target is this share of a thumb's height wide (mockup A)
    constexpr double kSpacesBottom = 24; // Spaces: main area to the usable bottom (no hint line)
    constexpr double kThumbBottom = 52;  // Task View: thumb row above the bottom edge (names sit above the cards)
    constexpr double kTitleGap = 32;     // thumb row to the title row (room for the names under the thumbs)
    constexpr double kTitleHeight = 52;  // title row to the main area
    constexpr double kMainBottom = 36;   // Task View: main area to the thumb row
    constexpr double kTaskViewTop = 76;
    constexpr double kTaskViewTitleY = 22;
    constexpr double kMinimizedOpacity = 0.55;

    constexpr double kStripGap = 60;      // between Strip rows: room for the next row's name above it (prototype)
    constexpr double kStripFilterTop = 8; // Strip: filter field top, the center island's row
    // Grid: filter field above the usable bottom (mockup B); the top of the screen holds the center island.
    constexpr double kFilterBottom = 24;
    constexpr double kFieldHeight = 32;
    constexpr double kPeekOpacity = 0.6;     // Strip: a neighbouring workspace row one step away (mockup B)
    constexpr double kStripPlusShare = 0.45; // the Strip's New row is this share of a row's extent along the axis
    constexpr double kDimmed = 0.5;
    constexpr double kHiddenFade = 3; // a window the placement hides is gone a third of the way into the open

    constexpr double kGridMaxWidthShare = 0.2875; // 368 px tiles on a 1280 px output
    constexpr double kGridGapX = 28;
    constexpr double kGridGapY = 52;
    constexpr double kGridTop = 64;
    constexpr double kGridChrome = 110; // filter row + hint
    constexpr size_t kGridColumns = 3;

    WindowPlace win(uint32_t id, size_t workspace, const Rect& rect, double opacity, bool dimmed = false) {
      WindowPlace w;
      w.id = id;
      w.workspace = workspace;
      w.rect = rect;
      w.opacity = opacity;
      w.dimmed = dimmed;
      return w;
    }

    Fit thumbFit(const Rect& card, double outputWidth) { return {card.w / outputWidth, card.x, card.y}; }

    // The part of `desk` outside the output, so a window shrunk into a workspace miniature stays inside it.
    std::optional<Insets> outputClip(const Rect& r, const Input& in) {
      const Insets cut{
          std::max(0.0, -r.y), std::max(0.0, r.x + r.w - in.width), std::max(0.0, r.y + r.h - in.height),
          std::max(0.0, -r.x)
      };
      return cut == Insets{} ? std::nullopt : std::optional<Insets>{cut};
    }

    // A member wholly off the output has nothing left after outputClip, so it is not drawn.
    double memberOpacity(const Member& m, const Input& in) {
      const bool onOutput =
          m.desk.x < in.width && m.desk.x + m.desk.w > 0 && m.desk.y < in.height && m.desk.y + m.desk.h > 0;
      return m.minimized || !onOutput ? 0.0 : 1.0;
    }

    // Members of a non-current workspace shrink into its thumb, cut to the output, so the thumb is a live miniature.
    void placeIntoCards(Placement& out, const Input& in) {
      for (const Card& card : out.cards) {
        if (card.workspace == in.current && card.kind == CardKind::Thumb) {
          continue;
        }
        const Fit fit = thumbFit(card.rect, in.width);
        for (const Member& m : in.workspaces[card.workspace].members) {
          WindowPlace w = win(m.id, card.workspace, mapRect(m.desk, fit), memberOpacity(m, in));
          w.clip = outputClip(m.desk, in);
          out.windows.push_back(w);
        }
      }
    }

    Placement placeBar(const Input& in, bool top) {
      Placement out;
      const size_t n = in.workspaces.size();
      const double avail = in.width - 2 * kMargin;
      double tw = top ? kSpacesThumbWidth : kTaskViewThumbWidth;
      tw = std::max(1.0, std::min(tw, (avail - static_cast<double>(n) * kThumbGap) / static_cast<double>(n + 1)));
      const double th = tw * in.height / in.width;
      const double plusW = top ? std::round(th * kPlusShare) : tw;
      const double total = static_cast<double>(n) * tw + plusW + static_cast<double>(n) * kThumbGap;
      const double y = top ? kThumbTop : in.height - th - kThumbBottom;
      double x = (in.width - total) / 2;
      for (size_t i = 0; i < n; ++i) {
        out.cards.push_back({i, {x, y, tw, th}, CardKind::Thumb});
        x += tw + kThumbGap;
      }
      out.plus = Rect{x, y, plusW, th};

      const double titleY = top ? y + th + kTitleGap : kTaskViewTitleY;
      const double mainY = top ? titleY + kTitleHeight : kTaskViewTop;
      const double mainBottom = top ? in.usable.y + in.usable.h - kSpacesBottom : y - kMainBottom;
      const Rect main{kMargin, mainY, in.width - 2 * kMargin, std::max(1.0, mainBottom - mainY)};
      out.main = main;
      out.mainFit = fitIn({0, 0, in.width, in.height}, main);
      out.title = Point{kMargin, titleY};
      // Spaces centres the field under the thumbs (mockup A); Task View keeps it at the trailing edge.
      out.filter = Point{top ? (in.width - kFilterWidth) / 2 : in.width - kMargin - kFilterWidth, titleY};

      if (in.current < n) {
        const Workspace& ws = in.workspaces[in.current];
        for (const Member& m : ws.members) {
          const double x0 = std::max(0.0, m.desk.x);
          const double y0 = std::max(0.0, m.desk.y);
          const double x1 = std::min(in.width, m.desk.x + m.desk.w);
          const double y1 = std::min(in.height, m.desk.y + m.desk.h);
          if (!m.minimized && x1 > x0 && y1 > y0) {
            out.currentDesk.push_back({x0, y0, x1 - x0, y1 - y0});
          }
        }
        if (!top || ws.arrangement == Arrangement::Free) {
          std::vector<ExposeItem> items;
          items.reserve(ws.members.size());
          for (const Member& m : ws.members) {
            items.push_back({m.id, m.desk});
          }
          const std::vector<ExposeItem> spread = exposeGrid(items, main);
          for (size_t i = 0; i < spread.size(); ++i) {
            out.windows.push_back(
                win(spread[i].id, in.current, spread[i].rect, ws.members[i].minimized ? kMinimizedOpacity : 1.0)
            );
          }
        } else {
          const Rect& u = in.usable;
          Rect content = u;
          if (ws.arrangement == Arrangement::Scrolling) {
            double x0 = u.x;
            double x1 = u.x + u.w;
            for (const Member& m : ws.members) {
              if (!m.minimized) {
                x0 = std::min(x0, m.desk.x);
                x1 = std::max(x1, m.desk.x + m.desk.w);
              }
            }
            content = {x0 - 8, u.y - 8, x1 - x0 + 16, u.h + 16};
          }
          const Fit fit = fitIn(content, main);
          out.mainFit = fit;
          if (ws.arrangement == Arrangement::Scrolling) {
            out.viewport = mapRect({u.x - 4, u.y - 4, u.w + 8, u.h + 8}, fit);
          }
          const Fit thumb = thumbFit(out.cards[in.current].rect, in.width);
          for (const Member& m : ws.members) {
            if (m.minimized) {
              out.windows.push_back(win(m.id, in.current, mapRect(m.desk, thumb), 0.0));
              continue;
            }
            const bool away = ws.arrangement == Arrangement::Scrolling && offscreen(m.desk, in.width);
            out.windows.push_back(win(m.id, in.current, mapRect(m.desk, fit), 1.0, away));
          }
        }
      }
      placeIntoCards(out, in);
      return out;
    }

    // Rows are whole pixels, so a column scrolled by whole rows puts every row on the pixel grid.
    Placement placeStrip(const Input& in) {
      Placement out;
      const size_t n = in.workspaces.size();
      const double s = std::clamp(in.stripScale, 0.05, 1.0);
      const double fw = std::max(1.0, std::round(in.width * s));
      const double fh = std::max(1.0, std::round(in.height * s));
      const double fx = std::round((in.width - fw) / 2);
      const double fy = std::round((in.height - fh) / 2);
      const double step = (in.horizontal ? fw : fh) + kStripGap;
      const double row = in.stripRow.value_or(static_cast<double>(in.current));
      const auto frameAt = [&](double i) {
        const double offset = (i - row) * step;
        return in.horizontal ? Rect{fx + offset, fy, fw, fh} : Rect{fx, fy + offset, fw, fh};
      };
      for (size_t i = 0; i < n; ++i) {
        const Rect frame = frameAt(static_cast<double>(i));
        out.cards.push_back({i, frame, CardKind::Frame});
        const Fit fit{s, frame.x, frame.y};
        const double peek = 1.0 - (1.0 - kPeekOpacity) * std::min(1.0, std::abs(static_cast<double>(i) - row));
        for (const Member& m : in.workspaces[i].members) {
          const bool away = in.workspaces[i].arrangement == Arrangement::Scrolling && offscreen(m.desk, in.width);
          out.windows.push_back(win(m.id, i, mapRect(m.desk, fit), m.minimized ? 0.0 : peek, away));
        }
      }
      Rect plus = frameAt(static_cast<double>(n));
      (in.horizontal ? plus.w : plus.h) *= kStripPlusShare;
      out.plus = plus;
      // Top trailing corner, level with the center island: the side bar islands step aside while the overview is open,
      // and no row reaches there at any zoom (the bottom holds the next row).
      out.filter = Point{in.width - kMargin - kFilterWidth, kStripFilterTop};
      return out;
    }

    double lerp(double a, double b, double t) { return a + (b - a) * t; }

    Insets lerpInsets(const Insets& a, const Insets& b, double t) {
      return {lerp(a.top, b.top, t), lerp(a.right, b.right, t), lerp(a.bottom, b.bottom, t), lerp(a.left, b.left, t)};
    }

    // Spaces and Task View between two chosen workspaces: every window and card moves from its place in `a` to its
    // place in `b`; the chrome jumps at the halfway point.
    Placement blend(const Placement& a, const Placement& b, double t) {
      Placement out = t < 0.5 ? a : b;
      out.windows.clear();
      for (const WindowPlace& from : a.windows) {
        const WindowPlace* to = findWindow(b, from.id);
        if (to == nullptr) {
          out.windows.push_back(from);
          continue;
        }
        WindowPlace w = t < 0.5 ? from : *to;
        w.rect = lerpRect(from.rect, to->rect, t);
        w.opacity = lerp(from.opacity, to->opacity, t);
        if (from.clip || to->clip) {
          w.clip = lerpInsets(from.clip.value_or(Insets{}), to->clip.value_or(Insets{}), t);
        }
        out.windows.push_back(w);
      }
      for (size_t i = 0; i < out.cards.size() && i < a.cards.size() && i < b.cards.size(); ++i) {
        out.cards[i].rect = lerpRect(a.cards[i].rect, b.cards[i].rect, t);
      }
      if (a.viewport && b.viewport) {
        out.viewport = lerpRect(*a.viewport, *b.viewport, t);
      }
      return out;
    }

    Placement placeGrid(const Input& in) {
      Placement out;
      const size_t n = in.workspaces.size();
      const size_t cols = std::min(kGridColumns, n + 1);
      const size_t rows = (n + 1 + cols - 1) / cols;
      const double availH = in.height - kGridChrome;
      const double byHeight =
          (availH - static_cast<double>(rows - 1) * kGridGapY) / static_cast<double>(rows) * in.width / in.height;
      const double byWidth =
          (in.width - 2 * kMargin - static_cast<double>(cols - 1) * kGridGapX) / static_cast<double>(cols);
      const double tw = std::max(1.0, std::min({in.width * kGridMaxWidthShare, byHeight, byWidth}));
      const double th = tw * in.height / in.width;
      const double x0 = (in.width - (static_cast<double>(cols) * tw + static_cast<double>(cols - 1) * kGridGapX)) / 2;
      const double y0 = kGridTop
          + std::max(0.0, (availH - (static_cast<double>(rows) * th + static_cast<double>(rows - 1) * kGridGapY)) / 2);
      const auto cell = [&](size_t i) {
        return Rect{
            x0 + static_cast<double>(i % cols) * (tw + kGridGapX),
            y0 + static_cast<double>(i / cols) * (th + kGridGapY), tw, th
        };
      };
      for (size_t i = 0; i < n; ++i) {
        const Rect tile = cell(i);
        out.cards.push_back({i, tile, CardKind::Tile});
        const Fit fit = thumbFit(tile, in.width);
        for (const Member& m : in.workspaces[i].members) {
          WindowPlace w = win(m.id, i, mapRect(m.desk, fit), memberOpacity(m, in));
          w.clip = outputClip(m.desk, in);
          out.windows.push_back(w);
        }
      }
      out.plus = cell(n);
      out.filter = Point{in.width / 2 - kFilterWidth / 2, in.usable.y + in.usable.h - kFilterBottom - kFieldHeight};
      return out;
    }

  } // namespace

  std::string_view styleName(Style style) { return kStyleNames[static_cast<size_t>(style)]; }

  std::optional<Style> styleNamed(std::string_view name) {
    for (size_t i = 0; i < kStyleNames.size(); ++i) {
      if (kStyleNames[i] == name) {
        return static_cast<Style>(i);
      }
    }
    return std::nullopt;
  }

  Placement place(Style style, const Input& input) {
    if (input.width <= 0 || input.height <= 0) {
      return {};
    }
    switch (style) {
    case Style::Spaces:
      return placeBar(input, true);
    case Style::TaskView:
      return placeBar(input, false);
    case Style::Strip:
      return placeStrip(input);
    case Style::Grid:
      return placeGrid(input);
    }
    return {};
  }

  const WindowPlace* findWindow(const Placement& placement, uint32_t id) {
    const auto it = std::ranges::find(placement.windows, id, &WindowPlace::id);
    return it == placement.windows.end() ? nullptr : &*it;
  }

  Placement placeAt(Style style, Input input, double row) {
    const size_t n = input.workspaces.size();
    if (n == 0) {
      return place(style, input);
    }
    row = std::clamp(row, 0.0, static_cast<double>(n - 1));
    input.current = static_cast<size_t>(std::lround(row));
    input.stripRow = row;
    if (style != Style::Spaces && style != Style::TaskView) {
      return place(style, input);
    }
    const double lower = std::floor(row);
    const double t = row - lower;
    input.current = static_cast<size_t>(lower);
    const Placement a = place(style, input);
    if (t < 1e-6 || input.current + 1 >= n) {
      return a;
    }
    input.current += 1;
    return blend(a, place(style, input), t);
  }

  Rect deskRect(const Input& input, const Rect& desk, size_t workspace, double row) {
    const double offset = (static_cast<double>(workspace) - row) * (input.horizontal ? input.width : input.height);
    return input.horizontal ? Rect{desk.x + offset, desk.y, desk.w, desk.h}
                            : Rect{desk.x, desk.y + offset, desk.w, desk.h};
  }

  Rect cardDeskRect(Style style, const Input& input, const Card& card, double row) {
    if (card.kind == CardKind::Thumb) {
      const double y = style == Style::Spaces ? -card.rect.h : input.height;
      return {card.rect.x, y, card.rect.w, card.rect.h};
    }
    return deskRect(input, {0, 0, input.width, input.height}, card.workspace, row);
  }

  double windowOpacity(const WindowPlace& place, bool minimized, double p) {
    p = std::clamp(p, 0.0, 1.0);
    double opacity = 1;
    if (minimized) {
      opacity = place.opacity * p;
    } else if (place.opacity <= 0) {
      opacity = 1 - std::min(1.0, p * kHiddenFade);
    } else {
      opacity = lerp(1, place.opacity, p);
    }
    return place.dimmed ? opacity * lerp(1, kDimmed, p) : opacity;
  }

} // namespace andromeda::ov
