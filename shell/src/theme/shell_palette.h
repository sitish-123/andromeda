#pragma once

#include "theme/fixed_palette.h"
#include "theme/palette.h"
#include "ui/palette.h"

namespace noctalia::theme {

  // Text-safe form of an accent fill: its lightness moves until it reads at 4.5:1 on `plane`.
  [[nodiscard]] ::Color accentTextOn(const ::Color& accentFill, const ::Color& plane);

  // on_accent kept at 4.5:1 on the accent fill it sits on.
  [[nodiscard]] ::Color onAccentOn(const ::Color& preferred, const ::Color& accentFill);

  // Recolours the token palette from the chosen palette: accent fill, accent_text, tints and solids.
  // Labels move only as far as needed for 4.5:1 on the new plane; every other role keeps its token value.
  // A palette with one empty mode uses the other mode for both schemes. The caller keeps the Andromeda palette
  // (the tokens themselves) out of this function.
  [[nodiscard]] ::Palette recolorShellPalette(const ::Palette& tokens, const GeneratedPalette& generated, bool light);

} // namespace noctalia::theme
