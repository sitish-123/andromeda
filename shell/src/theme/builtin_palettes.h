#pragma once

#include "theme/fixed_palette.h"

#include <string_view>

namespace noctalia::theme {

  struct FixedPaletteMode {
    FixedPalette palette;
    TerminalPalette terminal;
  };

  struct BuiltinPalette {
    std::string_view name;
    FixedPaletteMode dark;
    FixedPaletteMode light;
  };

  // The palette takes its colors from the design tokens; call on the main loop after the tokens reload.
  void refreshBuiltinPalettes();
  const BuiltinPalette& andromedaPalette();

  GeneratedPalette expandBuiltinPalette(const BuiltinPalette& palette);

} // namespace noctalia::theme
