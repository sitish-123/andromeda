#pragma once

#include "ui/controls/button.h"
#include "ui/style.h"

namespace panel_button_style {

  inline void configureHeaderIconButton(Button& button, float scale) {
    button.setVariant(ButtonVariant::Default);
    // Icon-only header buttons (back, close, header actions) are 32 circles (rulebook 9.5), as in macOS and iOS sheets.
    button.setGlyphSize(Tokens::sizeSymbol * scale);
    button.setMinWidth(Tokens::sizeControlM * scale);
    button.setMinHeight(Tokens::sizeControlM * scale);
    button.setPadding(Tokens::spaceP8 * scale);
    button.setRadius(Tokens::sizeControlM * 0.5F * scale);
  }

} // namespace panel_button_style
