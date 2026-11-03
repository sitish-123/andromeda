#pragma once

#include "render/core/color.h"
#include "ui/signal.h"
#include "ui/style.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

// Shell color roles: the [scheme.*] roles of protocol/design-tokens.toml that the shell draws with.
enum class ColorRole : std::uint8_t {
  Label1,
  Label2,
  Fill1,
  Fill2,
  Fill3,
  SolidThin,
  SolidRegular,
  Stroke,
  Accent,
  OnAccent,
  Danger,
  Shadow,
  TintRegular,
  TintThick,
  Label3,
  AccentText,
  DangerText,
  Separator,
  Lens,
  LensEdge,
  Warning,
  Success,
  SliderFill,
  OnSliderFill,
  SliderTrack,
  SliderEdge,
  Popover,
  // Internal: the modal dim layer. No token entry, so the legacy name "scrim" still maps to Shadow.
  Scrim,
};

struct ColorRoleToken {
  ColorRole role;
  std::string_view token;
};

inline constexpr std::array<ColorRoleToken, 27> kColorRoleTokens = {{
    {ColorRole::Label1, "label_1"},
    {ColorRole::Label2, "label_2"},
    {ColorRole::Fill1, "fill_1"},
    {ColorRole::Fill2, "fill_2"},
    {ColorRole::Fill3, "fill_3"},
    {ColorRole::SolidThin, "solid_thin"},
    {ColorRole::SolidRegular, "solid_regular"},
    {ColorRole::Stroke, "stroke"},
    {ColorRole::Accent, "accent"},
    {ColorRole::OnAccent, "on_accent"},
    {ColorRole::Danger, "danger"},
    {ColorRole::Shadow, "shadow"},
    {ColorRole::TintRegular, "tint_regular"},
    {ColorRole::TintThick, "tint_thick"},
    {ColorRole::Label3, "label_3"},
    {ColorRole::AccentText, "accent_text"},
    {ColorRole::DangerText, "danger_text"},
    {ColorRole::Separator, "separator"},
    {ColorRole::Lens, "lens"},
    {ColorRole::LensEdge, "lens_edge"},
    {ColorRole::Warning, "warning"},
    {ColorRole::Success, "success"},
    {ColorRole::SliderFill, "slider_fill"},
    {ColorRole::OnSliderFill, "on_slider_fill"},
    {ColorRole::SliderTrack, "slider_track"},
    {ColorRole::SliderEdge, "slider_edge"},
    {ColorRole::Popover, "popover"},
}};

[[nodiscard]] constexpr Color clearColor() noexcept { return rgba(0.0F, 0.0F, 0.0F, 0.0F); }

struct ColorSpec {
  std::optional<ColorRole> role;
  Color fixed = clearColor();
  float alpha = 1.0F;
};

constexpr bool operator==(const ColorSpec& a, const ColorSpec& b) noexcept {
  return a.role == b.role && a.fixed == b.fixed && a.alpha == b.alpha;
}

[[nodiscard]] constexpr ColorSpec clearColorSpec() noexcept {
  return ColorSpec{.role = std::nullopt, .fixed = clearColor(), .alpha = 1.0F};
}

// Multiplies a spec's alpha, keeping any color role intact so theme switches still resolve.
// Distinct from withAlpha(Color, float) in render/core/color.h, which replaces the alpha of an
// already-resolved color and therefore cannot preserve the role.
[[nodiscard]] constexpr ColorSpec scaleAlpha(const ColorSpec& color, float factor) noexcept {
  ColorSpec out = color;
  out.alpha = std::clamp(out.alpha * std::clamp(factor, 0.0F, 1.0F), 0.0F, 1.0F);
  return out;
}

// Runtime UI palette: one color per ColorRole, filled from the design-token scheme.
struct Palette {
  Color label1;
  Color label2;
  Color fill1;
  Color fill2;
  Color fill3;
  Color solidThin;
  Color solidRegular;
  Color stroke;
  Color accent;
  Color onAccent;
  Color danger;
  Color shadow;
  Color tintRegular;
  Color tintThick;
  Color label3;
  Color accentText;
  Color dangerText;
  Color separator;
  Color lens;
  Color lensEdge;
  Color warning;
  Color success;
  Color sliderFill;
  Color onSliderFill;
  Color sliderTrack;
  Color sliderEdge;
  Color popover;
  Color scrim;

  bool operator==(const Palette&) const = default;
};

extern Palette palette;

[[nodiscard]] const Color& colorForRole(ColorRole role) noexcept;
[[nodiscard]] Color colorForRole(ColorRole role, float alpha) noexcept;
[[nodiscard]] std::optional<ColorRole> colorRoleFromToken(std::string_view token);
// Plugin-facing lookup. Also accepts the retired `surface_variant` and `outline` roles, warning once per name.
[[nodiscard]] std::optional<ColorRole> colorRoleFromPluginToken(std::string_view token);
[[nodiscard]] std::string_view colorRoleToken(ColorRole role) noexcept;
[[nodiscard]] ColorSpec colorSpecFromRole(ColorRole role, float alpha = 1.0F) noexcept;
// Glass material fill: the tint over the compositor blur, or the opaque solid when transparency is reduced.
// The tint_regular token's alpha; the palette color is opaque so a caller's opacity replaces it, never multiplies it.
inline constexpr float kTintRegularAlpha = 0.70F;
[[nodiscard]] inline ColorRole materialRole() noexcept {
  return Style::reduceTransparency() ? ColorRole::SolidRegular : ColorRole::TintRegular;
}
// Panel and sheet material: tint_thick. A caller opacity is relative to tint_regular (0.70 = the token alpha) and is rescaled to thick.
inline constexpr float kTintThickAlpha = 0.92F;
[[nodiscard]] inline ColorRole panelMaterialRole() noexcept {
  return Style::reduceTransparency() ? ColorRole::SolidRegular : ColorRole::TintThick;
}
[[nodiscard]] inline float panelMaterialAlpha(float opacity) noexcept {
  return opacity >= 1.0F ? 1.0F : std::min(1.0F, opacity * (kTintThickAlpha / kTintRegularAlpha));
}
[[nodiscard]] ColorSpec fixedColorSpec(const Color& color) noexcept;
[[nodiscard]] Color resolveColorSpec(const ColorSpec& color) noexcept;
[[nodiscard]] bool isLightPalette() noexcept;
// Set by ThemeService from resolved light/dark mode (not inferred from palette colors).
[[nodiscard]] bool isResolvedLightTheme() noexcept;
void setResolvedThemeLight(bool light) noexcept;

void setPalette(const Palette& p);

[[nodiscard]] Palette tokenPalette(bool light) noexcept;

// Snapshot accessor safe to call from non-main threads (e.g. plugin script workers).
[[nodiscard]] Color colorForRoleSnapshot(ColorRole role);

[[nodiscard]] inline ColorSpec scrollbarTrackColor() noexcept {
  return colorSpecFromRole(ColorRole::Stroke, Style::disabledOutlineAlpha);
}
[[nodiscard]] inline ColorSpec scrollbarThumbColor() noexcept {
  return colorSpecFromRole(ColorRole::Label2, 0.5F);
}
[[nodiscard]] inline ColorSpec focusRingColorSpec() noexcept { return colorSpecFromRole(ColorRole::Accent); }

// Fired after setPalette() writes. Controls subscribe in their constructor
// and re-apply palette-derived colors to their scene nodes on each emit.
Signal<>& paletteChanged();

// Linearly interpolates each field of two palettes in sRGB space. Used by
// ThemeService to drive smooth cross-fade transitions on theme changes.
Palette lerpPalette(const Palette& a, const Palette& b, float t);
