#include "ui/palette.h"

#include "core/log.h"
#include "render/core/color.h"
#include "design_tokens.h"
#include "util/string_utils.h"

#include <mutex>
#include <string>

Palette palette = tokenPalette(false);
bool g_resolvedThemeLight = false;

namespace {

  // Serializes setPalette() writes against colorForRoleSnapshot() reads from the
  // script runtime thread. All other palette reads (colorForRole, rendering) stay
  // on the main thread, so this holds only while setPalette() is main-thread-only.
  std::mutex g_paletteMutex;

  std::string normalizedRoleToken(std::string_view token) {
    std::string normalized = StringUtils::trim(token);
    StringUtils::toLowerInPlace(normalized);
    return normalized;
  }

  constexpr auto kLog = Logger("palette");

  // Material role names from older configs and plugins.
  constexpr std::array<ColorRoleToken, 27> kMaterialRoleAliases = {{
      {ColorRole::Accent, "primary"},
      {ColorRole::OnAccent, "on_primary"},
      {ColorRole::Fill1, "primary_container"},
      {ColorRole::Label1, "on_primary_container"},
      {ColorRole::Accent, "secondary"},
      {ColorRole::OnAccent, "on_secondary"},
      {ColorRole::Fill1, "secondary_container"},
      {ColorRole::Label1, "on_secondary_container"},
      {ColorRole::Accent, "tertiary"},
      {ColorRole::OnAccent, "on_tertiary"},
      {ColorRole::Fill1, "tertiary_container"},
      {ColorRole::Label1, "on_tertiary_container"},
      {ColorRole::Danger, "error"},
      {ColorRole::OnAccent, "on_error"},
      {ColorRole::Fill1, "error_container"},
      {ColorRole::Label1, "on_error_container"},
      {ColorRole::SolidRegular, "surface"},
      {ColorRole::Label1, "on_surface"},
      {ColorRole::Label2, "on_surface_variant"},
      {ColorRole::SolidThin, "surface_container_low"},
      {ColorRole::Fill2, "surface_container"},
      {ColorRole::Fill3, "surface_container_high"},
      {ColorRole::Fill3, "surface_container_highest"},
      {ColorRole::Stroke, "outline_variant"},
      {ColorRole::Shadow, "scrim"},
      {ColorRole::Fill3, "hover"},
      {ColorRole::Label1, "on_hover"},
  }};

  Color tokenColor(std::uint32_t rgba) noexcept {
    return Color{static_cast<float>(rgba >> 24) / 255.0F, static_cast<float>((rgba >> 16) & 0xffU) / 255.0F,
                 static_cast<float>((rgba >> 8) & 0xffU) / 255.0F, static_cast<float>(rgba & 0xffU) / 255.0F};
  }

} // namespace

const Color& colorForRole(ColorRole role) noexcept {
  switch (role) {
  case ColorRole::Label1:
    return palette.label1;
  case ColorRole::Label2:
    return palette.label2;
  case ColorRole::Fill1:
    return palette.fill1;
  case ColorRole::Fill2:
    return palette.fill2;
  case ColorRole::Fill3:
    return palette.fill3;
  case ColorRole::SolidThin:
    return palette.solidThin;
  case ColorRole::SolidRegular:
    return palette.solidRegular;
  case ColorRole::Stroke:
    return palette.stroke;
  case ColorRole::Accent:
    return palette.accent;
  case ColorRole::OnAccent:
    return palette.onAccent;
  case ColorRole::Danger:
    return palette.danger;
  case ColorRole::Shadow:
    return palette.shadow;
  case ColorRole::TintRegular:
    return palette.tintRegular;
  case ColorRole::Label3:
    return palette.label3;
  }

  return palette.label1;
}

Color colorForRole(ColorRole role, float alpha) noexcept {
  Color color = colorForRole(role);
  color.a *= alpha;
  return color;
}

std::optional<ColorRole> colorRoleFromToken(std::string_view token) {
  const std::string normalized = normalizedRoleToken(token);
  for (const auto& option : kColorRoleTokens) {
    if (normalized == option.token) {
      return option.role;
    }
  }
  for (const auto& option : kMaterialRoleAliases) {
    if (normalized == option.token) {
      return option.role;
    }
  }
  return std::nullopt;
}

std::optional<ColorRole> colorRoleFromPluginToken(std::string_view token) {
  if (auto role = colorRoleFromToken(token)) {
    return role;
  }
  const std::string normalized = normalizedRoleToken(token);
  if (normalized == "surface_variant") {
    static std::once_flag warned;
    std::call_once(warned, [] {
      kLog.warn(R"(plugin color role "surface_variant" is deprecated; use "fill_2")");
    });
    return ColorRole::Fill2;
  }
  if (normalized == "outline") {
    static std::once_flag warned;
    std::call_once(warned, [] { kLog.warn(R"(plugin color role "outline" is deprecated; use "stroke")"); });
    return ColorRole::Stroke;
  }
  return std::nullopt;
}

std::string_view colorRoleToken(ColorRole role) noexcept {
  for (const auto& option : kColorRoleTokens) {
    if (option.role == role) {
      return option.token;
    }
  }
  return "label_1";
}

ColorSpec colorSpecFromRole(ColorRole role, float alpha) noexcept {
  return ColorSpec{.role = role, .fixed = clearColor(), .alpha = alpha};
}

ColorSpec fixedColorSpec(const Color& color) noexcept {
  return ColorSpec{.role = std::nullopt, .fixed = color, .alpha = 1.0F};
}

Color resolveColorSpec(const ColorSpec& color) noexcept {
  Color resolved = color.role.has_value() ? colorForRole(*color.role) : color.fixed;
  resolved.a *= color.alpha;
  return resolved;
}

bool isLightPalette() noexcept { return relativeLuminance(palette.solidRegular) > 0.179F; }

bool isResolvedLightTheme() noexcept { return g_resolvedThemeLight; }

void setResolvedThemeLight(bool light) noexcept { g_resolvedThemeLight = light; }

Signal<>& paletteChanged() {
  static Signal<> signal;
  return signal;
}

void setPalette(const Palette& p) {
  {
    std::scoped_lock lock(g_paletteMutex);
    if (palette == p) {
      return;
    }
    palette = p;
  }
  paletteChanged().emit();
}

Color colorForRoleSnapshot(ColorRole role) {
  std::scoped_lock lock(g_paletteMutex);
  return colorForRole(role);
}

Palette lerpPalette(const Palette& a, const Palette& b, float t) {
  return Palette{
      .label1 = lerpColor(a.label1, b.label1, t),
      .label2 = lerpColor(a.label2, b.label2, t),
      .fill1 = lerpColor(a.fill1, b.fill1, t),
      .fill2 = lerpColor(a.fill2, b.fill2, t),
      .fill3 = lerpColor(a.fill3, b.fill3, t),
      .solidThin = lerpColor(a.solidThin, b.solidThin, t),
      .solidRegular = lerpColor(a.solidRegular, b.solidRegular, t),
      .stroke = lerpColor(a.stroke, b.stroke, t),
      .accent = lerpColor(a.accent, b.accent, t),
      .onAccent = lerpColor(a.onAccent, b.onAccent, t),
      .danger = lerpColor(a.danger, b.danger, t),
      .shadow = lerpColor(a.shadow, b.shadow, t),
      .tintRegular = lerpColor(a.tintRegular, b.tintRegular, t),
      .label3 = lerpColor(a.label3, b.label3, t),
  };
}

Palette tokenPalette(bool light) noexcept {
  const auto& s = light ? Tokens::schemeLight : Tokens::schemeDark;
  return Palette{
      .label1 = tokenColor(s.label1),
      .label2 = tokenColor(s.label2),
      .fill1 = tokenColor(s.fill1),
      .fill2 = tokenColor(s.fill2),
      .fill3 = tokenColor(s.fill3),
      .solidThin = tokenColor(s.solidThin),
      .solidRegular = tokenColor(s.solidRegular),
      .stroke = tokenColor(s.stroke),
      .accent = tokenColor(s.accent),
      .onAccent = tokenColor(s.onAccent),
      .danger = tokenColor(s.danger),
      .shadow = tokenColor(s.shadow),
      .tintRegular = tokenColor(s.tintRegular | 0xffU), // callers carry the alpha: kTintRegularAlpha, glass opacity
      .label3 = tokenColor(s.label3),
  };
}
