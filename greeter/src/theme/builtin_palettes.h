#pragma once

#include "ui/palette.h"

#include <span>
#include <string_view>

namespace noctalia::theme {

  struct TerminalAnsiColors {
    ::Color black;
    ::Color red;
    ::Color green;
    ::Color yellow;
    ::Color blue;
    ::Color magenta;
    ::Color cyan;
    ::Color white;
  };

  struct TerminalPalette {
    TerminalAnsiColors normal;
    TerminalAnsiColors bright;
    ::Color foreground;
    ::Color background;
    ::Color selectionFg;
    ::Color selectionBg;
    ::Color cursorText;
    ::Color cursor;
  };

  struct FixedPaletteMode {
    ::Palette palette;
    TerminalPalette terminal;
  };

  struct BuiltinPalette {
    std::string_view name;
    FixedPaletteMode dark;
    FixedPaletteMode light;
  };

  std::span<const BuiltinPalette> builtinPalettes();
  const BuiltinPalette* findBuiltinPalette(std::string_view name);

} // namespace noctalia::theme
