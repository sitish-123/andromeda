#include "overview/style.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace andromeda::ov {

  namespace {

    constexpr std::array<std::string_view, 4> kStyleNames = {"spaces", "taskview", "strip", "grid"};

    // Spaces + Task View: thumbs in one row, sized down when they would not fit between the margins.
    constexpr double kThumbGap = 18;
    constexpr double kSpacesThumbWidth = 176;
    constexpr double kTaskViewThumbWidth = 208;
    constexpr double kThumbTop = 20;    // Spaces: thumb row from the top edge
    constexpr double kThumbBottom = 52; // Task View: thumb row above the bottom edge (names sit above the cards)
    constexpr double kTitleGap = 32;    // thumb row to the title row (room for the names under the thumbs)
    constexpr double kTitleHeight = 52; // title row to the main area
    constexpr double kMainBottom = 36;  // main area to the hint / thumb row
    constexpr double kTaskViewTop = 76;
    constexpr double kTaskViewTitleY = 22;
    constexpr double kMinimizedOpacity = 0.55;

    constexpr double kStripScale = 0.36;
    constexpr double kStripRowGap = 60;
    constexpr double kStripFilterY = 14;
    constexpr double kPlusShare = 0.45; // the Strip's New row is this share of a row's height

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

    // Members of a non-current workspace fly into its thumb and fade, so closing lands them in place.
    void placeIntoCards(Placement& out, const Input& in) {
      for (const Card& card : out.cards) {
        if (card.workspace == in.current && card.kind == CardKind::Thumb) {
          continue;
        }
        const Fit fit = thumbFit(card.rect, in.width);
        for (const Member& m : in.workspaces[card.workspace].members) {
          out.windows.push_back(
              win(m.id, card.workspace, mapRect(m.desk, fit), card.kind == CardKind::Thumb ? 0.0 : 1.0)
          );
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
      const double total = static_cast<double>(n + 1) * tw + static_cast<double>(n) * kThumbGap;
      const double y = top ? kThumbTop : in.height - th - kThumbBottom;
      double x = (in.width - total) / 2;
      for (size_t i = 0; i < n; ++i) {
        out.cards.push_back({i, {x, y, tw, th}, CardKind::Thumb});
        x += tw + kThumbGap;
      }
      out.plus = Rect{x, y, tw, th};

      const double titleY = top ? y + th + kTitleGap : kTaskViewTitleY;
      const double mainY = top ? titleY + kTitleHeight : kTaskViewTop;
      const double mainBottom = top ? in.height - kMainBottom : y - kMainBottom;
      const Rect main{kMargin, mainY, in.width - 2 * kMargin, std::max(1.0, mainBottom - mainY)};
      out.main = main;
      out.title = Point{kMargin, titleY};
      out.filter = Point{in.width - kMargin - kFilterWidth, titleY};

      if (in.current < n) {
        const Workspace& ws = in.workspaces[in.current];
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
            content = {x0 - 8, u.y, x1 - x0 + 16, u.h};
          }
          const Fit fit = fitIn(content, main);
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

    Placement placeStrip(const Input& in) {
      Placement out;
      const size_t n = in.workspaces.size();
      const double fw = in.width * kStripScale;
      const double fh = in.height * kStripScale;
      const double fx = (in.width - fw) / 2;
      const double pitch = fh + kStripRowGap;
      const double row = in.stripRow.value_or(static_cast<double>(in.current));
      const auto rowY = [&](double i) { return (in.height - fh) / 2 + (i - row) * pitch; };
      for (size_t i = 0; i < n; ++i) {
        const Rect frame{fx, rowY(static_cast<double>(i)), fw, fh};
        out.cards.push_back({i, frame, CardKind::Frame});
        const Fit fit{kStripScale, frame.x, frame.y};
        for (const Member& m : in.workspaces[i].members) {
          const bool away = in.workspaces[i].arrangement == Arrangement::Scrolling && offscreen(m.desk, in.width);
          out.windows.push_back(win(m.id, i, mapRect(m.desk, fit), m.minimized ? 0.0 : 1.0, away));
        }
      }
      out.plus = Rect{fx, rowY(static_cast<double>(n)), fw, fh * kPlusShare};
      out.filter = Point{in.width / 2 - kFilterWidth / 2, kStripFilterY};
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
          WindowPlace w = win(m.id, i, mapRect(m.desk, fit), m.minimized ? 0.0 : 1.0);
          const Rect& r = m.desk;
          const Insets cut{
              std::max(0.0, -r.y), std::max(0.0, r.x + r.w - in.width), std::max(0.0, r.y + r.h - in.height),
              std::max(0.0, -r.x)
          };
          if (cut != Insets{}) {
            w.clip = cut;
          }
          out.windows.push_back(w);
        }
      }
      out.plus = cell(n);
      out.filter = Point{in.width / 2 - kFilterWidth / 2, kStripFilterY};
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

} // namespace andromeda::ov
