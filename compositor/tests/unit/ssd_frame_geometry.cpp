#include "scene/ssd_frame_geometry.h"

#include "check.h"

#include <algorithm>
#include <array>
#include <span>

extern "C" {
#include <wlr/util/edges.h>
}

using namespace andromeda;

namespace {
  constexpr std::array kAll{TitleBarButton::Minimize, TitleBarButton::Maximize, TitleBarButton::Close};
  constexpr std::span<const TitleBarButton> kNone{};
} // namespace

ANDROMEDA_TEST(rightButtonsPutLastListedOutermost) {
  const auto parts = titleBarLayout(480, 28, TitleBarButtonSide::Right, kAll);
  CHECK_EQ(parts.button(TitleBarButton::Close)->x, 456);
  CHECK_EQ(parts.button(TitleBarButton::Close)->y, 8);
  CHECK_EQ(parts.button(TitleBarButton::Close)->width, 12);
  CHECK_EQ(parts.button(TitleBarButton::Close)->height, 12);
  CHECK_EQ(parts.button(TitleBarButton::Maximize)->x, 436);
  CHECK_EQ(parts.button(TitleBarButton::Minimize)->x, 416);
  CHECK_EQ(parts.title.x, 12);
  CHECK_EQ(parts.title.width, 396);
}

ANDROMEDA_TEST(leftButtonsMirror) {
  const auto parts = titleBarLayout(480, 28, TitleBarButtonSide::Left, kAll);
  CHECK_EQ(parts.button(TitleBarButton::Close)->x, 12);
  CHECK_EQ(parts.button(TitleBarButton::Maximize)->x, 32);
  CHECK_EQ(parts.button(TitleBarButton::Minimize)->x, 52);
  CHECK_EQ(parts.title.x, 72);
  CHECK_EQ(parts.title.width, 396);
}

ANDROMEDA_TEST(omittedButtonsLeaveNoGapAndWidenTheTitle) {
  constexpr std::array order{TitleBarButton::Close, TitleBarButton::Minimize};
  const auto parts = titleBarLayout(480, 28, TitleBarButtonSide::Right, order);
  CHECK_EQ(parts.button(TitleBarButton::Minimize)->x, 456);
  CHECK_EQ(parts.button(TitleBarButton::Close)->x, 436);
  CHECK(!parts.button(TitleBarButton::Maximize).has_value());
  CHECK_EQ(parts.title.width, 416);
  for (const auto side : {TitleBarButtonSide::Right, TitleBarButtonSide::Left}) {
    const auto bare = titleBarLayout(480, 28, side, kNone);
    CHECK(!bare.button(TitleBarButton::Close).has_value());
    CHECK_EQ(bare.title.x, 12);
    CHECK_EQ(bare.title.width, 456);
  }
}

ANDROMEDA_TEST(lightsSitOnTheBarCenterTwelveFromTheEdgeAtEveryHeight) {
  for (int height = 16; height <= 128; ++height) {
    const auto parts = titleBarLayout(480, height, TitleBarButtonSide::Right, kAll);
    const wlr_box close = *parts.button(TitleBarButton::Close);
    const int expected = std::clamp(height * 2 / 5 / 2 * 2, 12, 16);
    CHECK_EQ(close.height, expected);
    CHECK_EQ(height - (2 * close.y + close.height), height % 2);
    CHECK_EQ(480 - close.x - close.width, 12);
  }
}

ANDROMEDA_TEST(buttonHitSpansTitleBarHeight) {
  const auto at = [](TitleBarButtonSide side, std::span<const TitleBarButton> shown, double x, double y) {
    return ssdFramePointAt(480, 300, 28, 2, true, side, shown, x, y).part;
  };
  for (const double y : {-28.0, -14.0, -0.1}) {
    CHECK_EQ(at(TitleBarButtonSide::Right, kAll, 466, y), SsdFramePart::Close);
    CHECK_EQ(at(TitleBarButtonSide::Right, kAll, 444, y), SsdFramePart::Maximize);
    CHECK_EQ(at(TitleBarButtonSide::Right, kAll, 422, y), SsdFramePart::Minimize);
    CHECK_EQ(at(TitleBarButtonSide::Left, kAll, 14, y), SsdFramePart::Close);
  }
  CHECK_EQ(at(TitleBarButtonSide::Right, kAll, 466, 0), SsdFramePart::None);
  CHECK_EQ(at(TitleBarButtonSide::Right, kAll, 200, -15), SsdFramePart::TitleBar);
  // A hidden button's place belongs to the bar.
  CHECK_EQ(at(TitleBarButtonSide::Right, kNone, 466, -15), SsdFramePart::TitleBar);
}

ANDROMEDA_TEST(resizeBandOnlyWhenResizable) {
  CHECK_EQ(ssdFramePointAt(480, 300, 28, 2, false, TitleBarButtonSide::Right, kAll, -3, 100).part, SsdFramePart::None);
  CHECK_EQ(
      ssdFramePointAt(480, 300, 28, 2, true, TitleBarButtonSide::Right, kAll, -3, 100).edges, uint32_t{WLR_EDGE_LEFT}
  );
  CHECK_EQ(ssdFramePointAt(480, 300, 28, 2, true, TitleBarButtonSide::Right, kAll, -8.1, 100).part, SsdFramePart::None);
}

ANDROMEDA_TEST(resizeBandWithoutTitleBar) {
  const auto at = [](bool resizable, double x, double y) {
    return ssdFramePointAt(480, 300, 0, 2, resizable, TitleBarButtonSide::Right, kAll, x, y);
  };
  CHECK_EQ(at(true, -3, 100).part, SsdFramePart::Resize);
  CHECK_EQ(at(true, -3, 100).edges, uint32_t{WLR_EDGE_LEFT});
  CHECK_EQ(at(true, 483, 100).edges, uint32_t{WLR_EDGE_RIGHT});
  CHECK_EQ(at(true, 200, -3).edges, uint32_t{WLR_EDGE_TOP});
  CHECK_EQ(at(true, 200, 303).edges, uint32_t{WLR_EDGE_BOTTOM});
  CHECK_EQ(at(true, -3, -3).edges, uint32_t{WLR_EDGE_LEFT | WLR_EDGE_TOP});
  CHECK_EQ(at(true, 483, 303).edges, uint32_t{WLR_EDGE_RIGHT | WLR_EDGE_BOTTOM});
  CHECK_EQ(at(false, -3, 100).part, SsdFramePart::None);
  CHECK_EQ(at(true, 200, 100).part, SsdFramePart::None);
  CHECK_EQ(at(true, 466, 0).part, SsdFramePart::None);
  CHECK_EQ(at(true, -8.1, 100).part, SsdFramePart::None);
}

ANDROMEDA_TEST(cornerExtensionAddsBothEdges) {
  CHECK_EQ(
      ssdFramePointAt(480, 300, 28, 2, true, TitleBarButtonSide::Right, kAll, -3, -15).edges,
      uint32_t{WLR_EDGE_LEFT | WLR_EDGE_TOP}
  );
  CHECK_EQ(
      ssdFramePointAt(480, 300, 28, 2, true, TitleBarButtonSide::Right, kAll, 470, 302).edges,
      uint32_t{WLR_EDGE_RIGHT | WLR_EDGE_BOTTOM}
  );
  CHECK_EQ(
      ssdFramePointAt(480, 300, 28, 2, true, TitleBarButtonSide::Right, kAll, 200, -31).edges, uint32_t{WLR_EDGE_TOP}
  );
}

ANDROMEDA_TEST(contentBoxInTileRemovesTitleBar) {
  const auto content = contentBoxInTile({8, 8, 1264, 704}, 28);
  CHECK_EQ(content.x, 8);
  CHECK_EQ(content.y, 36);
  CHECK_EQ(content.width, 1264);
  CHECK_EQ(content.height, 676);
  CHECK_EQ(contentBoxInTile({-200, 40, 400, 12}, 28).height, 1);
  CHECK_EQ(contentBoxInTile({8, 8, 1264, 704}, 0).height, 704);
}

int main() { return RUN_TESTS(); }

ANDROMEDA_TEST(windowsCellsSitFlushAtTheEdgeAndTakeThePointer) {
  const auto parts = titleBarLayout(480, 28, TitleBarButtonSide::Right, kAll, TitleBarStyle::Windows);
  CHECK_EQ(parts.cells[static_cast<size_t>(TitleBarButton::Close)]->x, 434);
  CHECK_EQ(parts.cells[static_cast<size_t>(TitleBarButton::Close)]->width, 46);
  CHECK_EQ(parts.cells[static_cast<size_t>(TitleBarButton::Minimize)]->x, 342);
  CHECK_EQ(parts.button(TitleBarButton::Close)->x, 452);
  CHECK_EQ(parts.button(TitleBarButton::Close)->width, 10);
  CHECK_EQ(parts.title.width, 326);
  const auto at = [](double x) {
    return ssdFramePointAt(480, 300, 28, 2, true, TitleBarButtonSide::Right, kAll, x, -1, TitleBarStyle::Windows).part;
  };
  CHECK_EQ(at(479), SsdFramePart::Close);
  CHECK_EQ(at(434), SsdFramePart::Close);
  CHECK_EQ(at(433), SsdFramePart::Maximize);
  CHECK_EQ(at(341), SsdFramePart::TitleBar);
}

ANDROMEDA_TEST(macosLightsSitTwelveInAndTheTitleCentresOnTheWholeBar) {
  const auto parts = titleBarLayout(480, 28, TitleBarButtonSide::Left, kAll, TitleBarStyle::Macos);
  CHECK_EQ(parts.button(TitleBarButton::Close)->x, 12);
  CHECK_EQ(parts.button(TitleBarButton::Close)->width, 12);
  CHECK_EQ(parts.button(TitleBarButton::Minimize)->x, 32);
  CHECK_EQ(parts.button(TitleBarButton::Maximize)->x, 52);
  CHECK(parts.centredTitle);
  CHECK_EQ(parts.title.x, 72);
  CHECK_EQ(parts.title.width, 336);
}
