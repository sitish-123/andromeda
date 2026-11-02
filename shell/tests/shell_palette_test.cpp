#include "theme/builtin_palettes.h"
#include "theme/color.h"
#include "theme/contrast.h"
#include "theme/fixed_palette.h"
#include "theme/palette_transform.h"
#include "theme/toolkit_theme_files.h"
#include "theme/shell_palette.h"
#include "ui/style.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <print>
#include <string>
#include <string_view>

namespace {

  using noctalia::theme::contrastRatio;

  bool expect(bool condition, std::string_view message) {
    if (!condition) {
      std::println(stderr, "shell_palette_test: FAIL: {}", message);
    }
    return condition;
  }

  noctalia::theme::Color toInt(const ::Color& c) {
    const auto b = [](float v) { return std::clamp(static_cast<int>(std::lround(v * 255.0F)), 0, 255); };
    return {b(c.r), b(c.g), b(c.b)};
  }

  double labelContrast(const ::Color& label, const ::Color& plane) {
    const ::Color eff{
        .r = label.r * label.a + plane.r * (1 - label.a),
        .g = label.g * label.a + plane.g * (1 - label.a),
        .b = label.b * label.a + plane.b * (1 - label.a),
        .a = 1.0F,
    };
    return contrastRatio(toInt(eff), toInt(plane));
  }

  noctalia::theme::FixedPalette wallpaperPalette(std::string_view primary, std::string_view surface) {
    const ::Color p = hex(primary);
    const ::Color s = hex(surface);
    return {
        .primary = p, .onPrimary = hex("#ffffff"), .secondary = p, .onSecondary = hex("#ffffff"),
        .tertiary = p, .onTertiary = hex("#ffffff"), .error = hex("#d93025"), .onError = hex("#ffffff"),
        .surface = s, .onSurface = hex("#222222"), .surfaceVariant = s, .onSurfaceVariant = hex("#333333"),
        .outline = hex("#777777"), .shadow = hex("#000000"), .hover = p, .onHover = hex("#ffffff"),
    };
  }

  noctalia::theme::GeneratedPalette generatedFor(std::string_view primary, std::string_view surface) {
    return {
        .dark = noctalia::theme::expandFixedPaletteMode(wallpaperPalette(primary, surface), true),
        .light = noctalia::theme::expandFixedPaletteMode(wallpaperPalette(primary, surface), false),
    };
  }

  // Every role the shell clamps, on every plane the shell draws.
  bool checkRoles(const ::Palette& out, const std::string& where) {
    bool ok = true;
    const std::array<::Color, 4> planes = {out.solidRegular, out.solidThin, out.tintRegular, out.tintThick};
    for (const auto& plane : planes) {
      ok = expect(labelContrast(out.label1, plane) >= 4.5, where + " label_1 on plane") && ok;
      ok = expect(labelContrast(out.label2, plane) >= 4.5, where + " label_2 on plane") && ok;
    }
    ok = expect(contrastRatio(toInt(out.accentText), toInt(out.solidRegular)) >= 4.5, where + " accent_text") && ok;
    ok = expect(contrastRatio(toInt(out.onAccent), toInt(out.accent)) >= 4.5, where + " on_accent") && ok;
    return ok;
  }

  constexpr std::array<std::string_view, 5> kSurfaces = {"#b9cf83", "#8a9a3c", "#e6e08a", "#3a4a14", "#777777"};
  constexpr std::array<std::string_view, 3> kAccents = {"#b9cf83", "#f0e442", "#777777"};

  bool checkOlivePalettesClamp() {
    bool ok = true;
    bool clampNeeded = false;
    for (const bool light : {false, true}) {
      const ::Palette tokens = tokenPalette(light);
      for (const auto surface : kSurfaces) {
        for (const auto accent : kAccents) {
          const std::string where = std::string(light ? "light " : "dark ") + std::string(surface) + "/" + std::string(accent);
          const ::Palette out = noctalia::theme::recolorShellPalette(tokens, generatedFor(accent, surface), light);
          ok = checkRoles(out, where) && ok;
          ok = expect(out.accent.r == hex(accent).r && out.accent.g == hex(accent).g, where + " accent fill kept") && ok;
          ok = expect(out.fill1 == tokens.fill1 && out.separator == tokens.separator, where + " token roles kept") && ok;
          clampNeeded = clampNeeded || labelContrast(tokens.label2, out.solidRegular) < 4.5;
        }
      }
    }
    return expect(clampNeeded, "token labels alone fail on some olive plane, so the clamp is exercised") && ok;
  }

  bool checkHighContrast() {
    bool ok = true;
    for (const bool light : {false, true}) {
      auto generated = generatedFor("#b9cf83", "#8a9a3c");
      noctalia::theme::applyHighContrast(generated);
      ::Palette out = noctalia::theme::recolorShellPalette(tokenPalette(light), generated, light);
      out.label2 = out.label1;
      ok = checkRoles(out, std::string(light ? "light" : "dark") + " high contrast") && ok;
    }
    return ok;
  }

  bool checkSingleModePalette() {
    bool ok = true;
    for (const bool lightPalette : {false, true}) {
      auto generated = generatedFor("#b9cf83", "#8a9a3c");
      (lightPalette ? generated.dark : generated.light).clear();
      for (const bool light : {false, true}) {
        const ::Palette tokens = tokenPalette(light);
        const ::Palette out = noctalia::theme::recolorShellPalette(tokens, generated, light);
        const std::string where = std::string(lightPalette ? "light-only" : "dark-only") + (light ? " in light" : " in dark");
        ok = expect(!(out == tokens), where + " is recoloured") && ok;
        ok = checkRoles(out, where) && ok;
      }
    }
    return ok;
  }

  bool checkAndromedaColoursUnderAnotherNameAreRecoloured() {
    bool ok = true;
    const auto generated = noctalia::theme::expandBuiltinPalette(*noctalia::theme::findBuiltinPalette("Andromeda"));
    for (const bool light : {false, true}) {
      const ::Palette tokens = tokenPalette(light);
      const ::Palette out = noctalia::theme::recolorShellPalette(tokens, generated, light);
      const std::string where = std::string(light ? "light" : "dark") + " Andromeda colours";
      ok = expect(!(out == tokens), where + " recolour is not a no-op") && ok;
      ok = checkRoles(out, where) && ok;
    }
    return ok;
  }

  bool checkWriterMatchesShell() {
    namespace fs = std::filesystem;
    bool ok = true;
    const fs::path root = fs::temp_directory_path() / "shell_palette_test";
    fs::remove_all(root);
    fs::create_directories(root / "data/andromeda/gtk");
    fs::create_directories(root / "config");
    for (const char* name : {"andromeda-gtk3-dark.css", "andromeda-gtk4-dark.css", "andromeda-gtk3-light.css",
                             "andromeda-gtk4-light.css"}) {
      std::ofstream(root / "data/andromeda/gtk" / name) << "/* base */\n";
    }
    setenv("XDG_DATA_HOME", (root / "data").c_str(), 1);
    setenv("XDG_DATA_DIRS", (root / "none").c_str(), 1);
    setenv("XDG_CONFIG_HOME", (root / "config").c_str(), 1);
    for (const bool light : {false, true}) {
      const ::Palette out =
          noctalia::theme::recolorShellPalette(tokenPalette(light), generatedFor("#b9cf83", "#8a9a3c"), light);
      ok = expect(toolkit_theme::write(light ? "light" : "dark", out.accent, out.accentText, out.onAccent), "writer ran")
          && ok;
      for (const char* dir : {"gtk-3.0", "gtk-4.0"}) {
        std::ifstream in(root / "config" / dir / "andromeda.css");
        std::stringstream text;
        text << in.rdbuf();
        const std::string expected = "@define-color accent_color " + formatRgbHex(out.accentText) + ";";
        ok = expect(text.str().find(expected) != std::string::npos, std::string(dir) + " accent_text equals the shell's")
            && ok;
      }
      ok = expect(contrastRatio(toInt(out.accentText), toInt(out.solidRegular)) >= 4.5, "shell accent_text clamped") && ok;
    }
    fs::remove_all(root);
    return ok;
  }

} // namespace

int main() {
  bool ok = checkOlivePalettesClamp();
  ok = checkHighContrast() && ok;
  ok = checkSingleModePalette() && ok;
  ok = checkAndromedaColoursUnderAnotherNameAreRecoloured() && ok;
  ok = checkWriterMatchesShell() && ok;
  return ok ? 0 : 1;
}
