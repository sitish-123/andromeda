#pragma once

#include "theme/fixed_palette.h"

#include <span>
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

  // Entry 0 (Andromeda) takes its colors from the design tokens; call on the main loop after the tokens reload.
  void refreshBuiltinPalettes();
  std::span<const BuiltinPalette> builtinPalettes();

  const BuiltinPalette* findBuiltinPalette(std::string_view name);
  GeneratedPalette expandBuiltinPalette(const BuiltinPalette& palette);

} // namespace noctalia::theme
