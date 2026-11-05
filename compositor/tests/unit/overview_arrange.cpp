#include "check.h"
#include "overview/arrange.h"
#include "overview/style.h"

#include <cmath>

using namespace andromeda::ov;

namespace {
  constexpr double kW = 1280;
  constexpr double kH = 800;
  const Rect kUsable{8, 44, kW - 16, kH - 44 - 84};

  bool inside(const Rect& r, const Rect& box, double slack = 0.5) {
    return r.x >= box.x - slack
        && r.y >= box.y - slack
        && r.x + r.w <= box.x + box.w + slack
        && r.y + r.h <= box.y + box.h + slack;
  }

  bool overlap(const Rect& a, const Rect& b) {
    return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
  }

  // The prototype's start state: Main scrolls three columns (the third off screen), Chat and Media hold two each.
  Input sample(Arrangement mainArrangement, size_t workspaces = 3) {
    Input in{kW, kH, kUsable, {}, 0, std::nullopt};
    Workspace main{mainArrangement, {{1, {8, 44, 628, 672}}, {2, {644, 44, 628, 672}}, {3, {1280, 44, 416, 672}}}};
    in.workspaces.push_back(main);
    uint32_t id = 10;
    for (size_t i = 1; i < workspaces; ++i) {
      in.workspaces.push_back({Arrangement::Scrolling, {{id++, {8, 44, 628, 672}}, {id++, {644, 44, 628, 672}}}});
    }
    return in;
  }
} // namespace

ANDROMEDA_TEST(spacesMatchesThePrototypeAt1280x800) {
  const Placement p = place(Style::Spaces, sample(Arrangement::Scrolling));
  CHECK_EQ(p.cards.size(), size_t{3});
  CHECK_EQ(p.cards[0].rect.y, 20.0);
  CHECK_EQ(p.cards[0].rect.w, 176.0);
  CHECK_EQ(p.cards[0].rect.h, 110.0);
  CHECK(p.title.has_value() && p.title->y == 162.0);
  CHECK(p.main.has_value() && p.main->y == 214.0);
  CHECK(p.plus.has_value() && p.plus->x == p.cards[2].rect.x + 176 + 18);
}

ANDROMEDA_TEST(spacesScrollingShowsTheWholeStripInsideTheMainArea) {
  const Placement p = place(Style::Spaces, sample(Arrangement::Scrolling));
  for (uint32_t id : {1U, 2U, 3U}) {
    const WindowPlace* w = findWindow(p, id);
    CHECK(w != nullptr && inside(w->rect, *p.main));
  }
  CHECK(!findWindow(p, 1)->dimmed);
  CHECK(findWindow(p, 3)->dimmed);
  CHECK(p.viewport.has_value() && inside(*p.viewport, *p.main));
  // The off-screen column sits right of the frame marking the screen.
  CHECK(findWindow(p, 3)->rect.x >= p.viewport->x + p.viewport->w - 4);
}

ANDROMEDA_TEST(otherWorkspacesFlyIntoTheirThumbAndFade) {
  for (Style style : {Style::Spaces, Style::TaskView}) {
    const Placement p = place(style, sample(Arrangement::Scrolling));
    for (const WindowPlace& w : p.windows) {
      if (w.workspace == 0) {
        continue;
      }
      CHECK_EQ(w.opacity, 0.0);
      CHECK(inside(w.rect, p.cards[w.workspace].rect));
    }
  }
}

ANDROMEDA_TEST(freeWorkspacesSpreadWithoutOverlap) {
  Input in = sample(Arrangement::Free);
  in.workspaces[0].members = {
      {1, {40, 70, 820, 560}}, {2, {380, 140, 780, 520}}, {3, {220, 300, 640, 380}}, {4, {200, 200, 500, 400}, true}
  };
  for (Style style : {Style::Spaces, Style::TaskView}) {
    const Placement p = place(style, in);
    for (uint32_t a = 1; a <= 4; ++a) {
      const WindowPlace* wa = findWindow(p, a);
      CHECK(wa != nullptr && inside(wa->rect, *p.main));
      for (uint32_t b = a + 1; b <= 4; ++b) {
        CHECK(!overlap(wa->rect, findWindow(p, b)->rect));
      }
    }
    CHECK_EQ(findWindow(p, 4)->opacity, 0.55);
  }
}

ANDROMEDA_TEST(exposeNeverUpscalesAndKeepsReadingOrder) {
  const ExposeItem items[] = {{1, {700, 60, 300, 200}}, {2, {60, 60, 300, 200}}};
  const auto out = exposeGrid(items, {0, 0, 1600, 900});
  CHECK(out[0].rect.w <= 300 * 0.7 + 1e-9);
  // Both reach the 0.7 cap in one column, so a second column adds no area and they stack; same 200 px band, so the
  // left window comes first (on top).
  CHECK_EQ(out[0].rect.x, out[1].rect.x);
  CHECK(out[1].rect.y < out[0].rect.y);
}

ANDROMEDA_TEST(taskViewKeepsScrollingWorkspacesSpreadOutToo) {
  const Placement p = place(Style::TaskView, sample(Arrangement::Scrolling));
  CHECK(!p.viewport.has_value());
  CHECK(p.cards[0].rect.y + p.cards[0].rect.h == kH - 52);
  CHECK(p.main.has_value() && p.main->y + p.main->h <= p.cards[0].rect.y);
}

ANDROMEDA_TEST(thumbsShrinkToFitManyWorkspaces) {
  const Placement p = place(Style::Spaces, sample(Arrangement::Scrolling, 10));
  CHECK(p.plus.has_value());
  CHECK(p.cards.front().rect.x >= kMargin - 0.5);
  CHECK(p.plus->x + p.plus->w <= kW - kMargin + 0.5);
}

ANDROMEDA_TEST(gridClipsEveryWindowToItsTile) {
  const Placement p = place(Style::Grid, sample(Arrangement::Scrolling, 4));
  CHECK(p.plus.has_value() && inside(*p.plus, {0, 0, kW, kH}));
  for (const WindowPlace& w : p.windows) {
    const Rect& tile = p.cards[w.workspace].rect;
    const double s = tile.w / kW;
    Rect shown = w.rect;
    if (w.clip) {
      shown = {
          shown.x + w.clip->left * s, shown.y + w.clip->top * s, shown.w - (w.clip->left + w.clip->right) * s,
          shown.h - (w.clip->top + w.clip->bottom) * s
      };
    }
    CHECK(inside(shown, tile));
  }
  CHECK(findWindow(p, 3)->clip.has_value());
}

ANDROMEDA_TEST(stripCentresTheChosenRowAndScrollsFractionally) {
  Input in = sample(Arrangement::Scrolling);
  in.current = 1;
  const Placement p = place(Style::Strip, in);
  const Rect& row = p.cards[1].rect;
  CHECK(std::abs((row.y + row.h / 2) - kH / 2) < 1e-9);
  in.stripRow = 1.5;
  const Placement half = place(Style::Strip, in);
  const double mid = (half.cards[1].rect.y + half.cards[1].rect.h + half.cards[2].rect.y) / 2;
  CHECK(std::abs(mid - kH / 2) < 1e-9);
}

int main() { return RUN_TESTS(); }
