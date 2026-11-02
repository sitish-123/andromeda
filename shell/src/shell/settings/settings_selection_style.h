#pragma once

#include "ui/controls/button.h"
#include "ui/palette.h"

namespace settings {

  // Every size of the Settings frame (sidebar, rows, cards, page) in unscaled design units; layout code
  // multiplies by the ui scale. A restyle edits this header only.
  namespace metrics {
    inline constexpr float kSidebarWidth = 248.0F;
    inline constexpr float kSidebarPaddingV = 12.0F;
    inline constexpr float kSidebarPaddingH = 10.0F;
    inline constexpr float kSidebarGap = 2.0F;
    inline constexpr float kSidebarGroupGap = 10.0F;
    inline constexpr float kSidebarRowHeight = 30.0F;
    inline constexpr float kSidebarRowPaddingH = 6.0F;
    inline constexpr float kSidebarRowGap = 8.0F;
    inline constexpr float kSidebarTile = 22.0F;
    inline constexpr float kSidebarTileGlyph = 14.0F;
    inline constexpr float kAccountAvatar = 36.0F;
    inline constexpr float kNewBarInputWidth = 120.0F;
    inline constexpr float kSearchFieldHeight = 30.0F;
    inline constexpr float kResultTile = 28.0F;
    inline constexpr float kEmptyStateMinHeight = 160.0F;
    inline constexpr float kTitleBarHeight = 52.0F;
    inline constexpr const float& kPagePadding = Tokens::componentPagePadding;
    inline constexpr float kCardBorderWidth = 0.5F;
    inline constexpr float kCardFillOpacity = 0.4F;
    inline constexpr float kRowSeparatorThickness = 0.5F;
    inline constexpr float kRowHeight = 40.0F;
    inline constexpr float kRowHeightTwoLine = 56.0F;
    inline constexpr float kPickerTileWidth = 96.0F;
    inline constexpr float kPickerTileHeight = 64.0F;
    inline constexpr float kPickerGap = 8.0F;
    inline constexpr float kPickerRadius = 12.0F;
  } // namespace metrics

  // The one place that decides how a selected item looks in Settings: the open sidebar page, the open bar row and
  // a search-result highlight. Switching the selection language is a change to this function only.
  struct SelectionStyle {
    ColorSpec rowFill;
    ColorSpec rowLabel;
    ColorSpec tileFill;
    ColorSpec tileGlyph;
    ColorSpec childFill;
    ColorSpec highlightFill;
  };

  [[nodiscard]] inline SelectionStyle selectionStyle() {
    return SelectionStyle{
        .rowFill = colorSpecFromRole(ColorRole::Accent),
        .rowLabel = colorSpecFromRole(ColorRole::OnAccent),
        .tileFill = colorSpecFromRole(ColorRole::OnAccent, 0.22F),
        .tileGlyph = colorSpecFromRole(ColorRole::OnAccent),
        .childFill = colorSpecFromRole(ColorRole::Fill3),
        .highlightFill = colorSpecFromRole(ColorRole::Accent, 0.14F),
    };
  }

  // Colours of the unselected frame parts.
  struct FrameColors {
    ColorSpec tileFill;
    ColorSpec tileGlyph;
    ColorSpec cardFill;
    ColorSpec cardBorder;
    ColorSpec avatarFill;
    ColorSpec avatarInitial;
  };

  [[nodiscard]] inline FrameColors frameColors() {
    return FrameColors{
        .tileFill = colorSpecFromRole(ColorRole::Fill3),
        .tileGlyph = colorSpecFromRole(ColorRole::Label1),
        .cardFill = colorSpecFromRole(ColorRole::Fill1),
        .cardBorder = colorSpecFromRole(ColorRole::Stroke),
        .avatarFill = colorSpecFromRole(ColorRole::Accent),
        .avatarInitial = colorSpecFromRole(ColorRole::OnAccent),
    };
  }

  // A button palette that keeps the selected look in every pointer state.
  [[nodiscard]] inline Button::ButtonPalette selectedRowPalette(const ColorSpec& fill, const ColorSpec& label) {
    const Button::ButtonStateColors state{.bg = fill, .border = clearColorSpec(), .label = label};
    return Button::ButtonPalette{.normal = state, .hover = state, .pressed = state, .disabled = state};
  }

} // namespace settings
