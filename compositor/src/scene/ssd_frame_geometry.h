#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

extern "C" {
#include <wlr/util/box.h>
}

namespace andromeda {
  enum class TitleBarButtonSide : uint8_t { Left, Right };
  // Andromeda: round buttons, title at the start. Macos: round buttons, centred title. Windows: full-height 40:36
  // button cells flush to the edge, the round mark centred in each, the cell filled on hover.
  enum class TitleBarStyle : uint8_t { Andromeda, Macos, Windows };
  enum class TitleBarButton : uint8_t { Minimize, Maximize, Close };
  inline constexpr size_t kTitleBarButtonCount = 3;
  enum class SsdFramePart : uint8_t { None, TitleBar, Close, Maximize, Minimize, Resize };

  constexpr int kTitleBarButtonGap = 8;
  constexpr int kTitleBarTitlePadding = 8;
  constexpr int kSsdResizeBand = 6;
  constexpr int kSsdResizeCorner = 20;

  struct TitleBarLayout {
    // Indexed by TitleBarButton; empty for a button the bar does not show.
    std::array<std::optional<wlr_box>, kTitleBarButtonCount> buttons{};
    // The pointer target of each button: the full bar height, as wide as the mark plus its gap or the Windows cell.
    std::array<std::optional<wlr_box>, kTitleBarButtonCount> cells{};
    wlr_box title{};
    bool centredTitle = false;
    [[nodiscard]] const std::optional<wlr_box>& button(TitleBarButton which) const {
      return buttons[static_cast<size_t>(which)];
    }
  };
  struct SsdFramePoint {
    SsdFramePart part = SsdFramePart::None;
    uint32_t edges = 0;
  };

  // About half the bar height, with the bar's parity so the margins above and below are whole and equal pixels.
  [[nodiscard]] int titleBarButtonDiameter(int height);
  // `shown` lists the buttons from the title outward, so the last one sits at the bar's edge on `side`.
  [[nodiscard]] TitleBarLayout
  titleBarLayout(int width, int height, TitleBarButtonSide side, std::span<const TitleBarButton> shown,
                 TitleBarStyle style = TitleBarStyle::Andromeda);
  // Coordinates relative to client content; the title bar lies above y = 0.
  [[nodiscard]] SsdFramePoint ssdFramePointAt(
      int contentWidth, int contentHeight, int titleBarHeight, int borderInset, bool resizable, TitleBarButtonSide side,
      std::span<const TitleBarButton> shown, double x, double y, TitleBarStyle style = TitleBarStyle::Andromeda
  );
  [[nodiscard]] wlr_box contentBoxInTile(const wlr_box& tile, int titleBarHeight);
} // namespace andromeda
