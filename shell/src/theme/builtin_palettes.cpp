#include "theme/builtin_palettes.h"

#include "theme/fixed_palette.h"
#include "ui/style.h"

#include <string_view>

namespace noctalia::theme {

  namespace {

    // The Andromeda palette comes from the design tokens (protocol/design-tokens.toml scheme.*).
    constexpr ::Color opaque(std::uint32_t rgba) {
      ::Color c = rgbaHex(rgba);
      c.a = 1.0F;
      return c;
    }

    // fg (with its alpha) composited over an opaque bg.
    constexpr ::Color over(std::uint32_t fg, std::uint32_t bg) {
      const ::Color f = rgbaHex(fg);
      const ::Color b = rgbaHex(bg);
      return ::Color{
          .r = f.r * f.a + b.r * (1.0F - f.a),
          .g = f.g * f.a + b.g * (1.0F - f.a),
          .b = f.b * f.a + b.b * (1.0F - f.a),
          .a = 1.0F,
      };
    }

    template <typename Scheme> constexpr FixedPalette tokenFixedPalette(const Scheme& t) {
      return FixedPalette{
          .primary = opaque(t.accent),
          .onPrimary = opaque(t.onAccent),
          .secondary = opaque(t.accent),
          .onSecondary = opaque(t.onAccent),
          .tertiary = opaque(t.accent),
          .onTertiary = opaque(t.onAccent),
          .error = opaque(t.danger),
          .onError = opaque(t.onAccent),
          .surface = opaque(t.window),
          .onSurface = over(t.label1, t.window),
          .surfaceVariant = opaque(t.card),
          .onSurfaceVariant = over(t.label2, t.window),
          .outline = over(t.separator, t.window),
          .shadow = opaque(t.shadow),
          .hover = opaque(t.accent),
          .onHover = opaque(t.onAccent),
      };
    }

    constexpr TerminalAnsiColors kAndromedaAnsiDark{
        .black = hex("#1e1e22"), .red = hex("#ff453a"), .green = hex("#32d74b"), .yellow = hex("#ffd60a"),
        .blue = hex("#4a80ff"), .magenta = hex("#bf5af2"), .cyan = hex("#64d2ff"), .white = hex("#f5f5f7"),
    };
    constexpr TerminalAnsiColors kAndromedaAnsiLight{
        .black = hex("#1d1d1f"), .red = hex("#d70015"), .green = hex("#248a3d"), .yellow = hex("#b25000"),
        .blue = hex("#2f6bff"), .magenta = hex("#8944ab"), .cyan = hex("#0071a4"), .white = hex("#f2f2f6"),
    };

    BuiltinPalette kAndromeda =
        {
            .name = "Andromeda",
            .dark =
                FixedPaletteMode{
                    .palette = tokenFixedPalette(Tokens::schemeDark),
                    .terminal =
                        TerminalPalette{
                            .normal = kAndromedaAnsiDark,
                            .bright = kAndromedaAnsiDark,
                            .foreground = hex("#f5f5f7"),
                            .background = hex("#1e1e22"),
                            .selectionFg = hex("#ffffff"),
                            .selectionBg = hex("#4a80ff"),
                            .cursorText = hex("#1e1e22"),
                            .cursor = hex("#f5f5f7"),
                        },
                },
            .light =
                FixedPaletteMode{
                    .palette = tokenFixedPalette(Tokens::schemeLight),
                    .terminal =
                        TerminalPalette{
                            .normal = kAndromedaAnsiLight,
                            .bright = kAndromedaAnsiLight,
                            .foreground = hex("#1d1d1f"),
                            .background = hex("#ffffff"),
                            .selectionFg = hex("#ffffff"),
                            .selectionBg = hex("#2f6bff"),
                            .cursorText = hex("#ffffff"),
                            .cursor = hex("#1d1d1f"),
                        },
                },
        };

  } // namespace

  void refreshBuiltinPalettes() {
    kAndromeda.dark.palette = tokenFixedPalette(Tokens::schemeDark);
    kAndromeda.light.palette = tokenFixedPalette(Tokens::schemeLight);
  }

  const BuiltinPalette& andromedaPalette() { return kAndromeda; }

  GeneratedPalette expandBuiltinPalette(const BuiltinPalette& palette) {
    auto generated = expandFixedPalettes(palette.dark.palette, palette.light.palette);
    applyTerminalPalette(generated.dark, palette.dark.terminal);
    applyTerminalPalette(generated.light, palette.light.terminal);
    return generated;
  }

} // namespace noctalia::theme
