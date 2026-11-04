#pragma once

#include "ui/controls/button.h"
#include "ui/palette.h"

namespace settings {

  // Every size of the Settings frame (sidebar, rows, cards, page) in unscaled design units; layout code
  // multiplies by the ui scale. A restyle edits this header only.
  namespace metrics {
    inline constexpr float kSidebarWidth = 240.0F;
    inline constexpr float kSidebarPaddingV = 8.0F;
    inline constexpr float kSidebarPaddingH = 8.0F;
    inline constexpr float kSidebarGap = 0.0F;
    inline constexpr float kSidebarGroupGap = 16.0F;
    inline constexpr float kSidebarRowHeight = 32.0F;
    inline constexpr float kSidebarRowPaddingH = 8.0F;
    inline constexpr float kSidebarRowGap = 8.0F;
    inline constexpr float kSidebarTile = 22.0F;
    inline constexpr float kSidebarTileGlyph = 16.0F;
    inline constexpr float kAccountAvatar = 24.0F;
    inline constexpr float kNewBarInputWidth = 120.0F;
    inline constexpr float kSearchFieldHeight = 32.0F;
    inline constexpr float kResultTile = 28.0F;
    inline constexpr float kEmptyStateMinHeight = 160.0F;
    inline constexpr float kTitleBarHeight = 40.0F;
    inline constexpr const float& kPagePadding = Tokens::componentPagePadding;
    inline constexpr float kCardBorderWidth = 0.5F;
    inline constexpr float kCardFillOpacity = 0.4F;
    inline constexpr float kRowSeparatorThickness = 1.0F;
    inline constexpr float kContentMaxWidth = 640.0F;
    inline constexpr float kRowHeight = 40.0F;
    inline constexpr float kRowHeightTwoLine = 56.0F;
    inline constexpr float kPickerTileWidth = 96.0F;
    inline constexpr float kPickerTileHeight = 64.0F;
    inline constexpr float kPickerGap = 8.0F;
    inline constexpr float kThumbHeight = 56.0F;
    inline constexpr float kKeycapHeight = 20.0F;
    inline constexpr float kSwatchSize = 20.0F;
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
        .rowFill = colorSpecFromRole(ColorRole::Lens),
        .rowLabel = colorSpecFromRole(ColorRole::Label1),
        .tileFill = clearColorSpec(),
        .tileGlyph = colorSpecFromRole(ColorRole::AccentText),
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
        .tileFill = clearColorSpec(),
        .tileGlyph = colorSpecFromRole(ColorRole::Label2),
        // Scheme token `card` (white in light, #28282c in dark): grouped cards read white on the window plane.
        .cardFill = fixedColorSpec(rgbaHex(isResolvedLightTheme() ? Tokens::schemeLight.card : Tokens::schemeDark.card)),
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
