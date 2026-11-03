#include "ui/style.h"

#include "design_tokens.h"
#include "ui/palette.h"

#include <algorithm>
#include <cmath>

namespace {

  float g_cornerRadiusScale = 1.0F;
  bool g_buttonBordersEnabled = true;
  bool g_inputBordersEnabled = true;
  bool g_popupBordersEnabled = true;
  bool g_rtl = false;
  bool g_popupShadowsEnabled = true;
  bool g_cardBordersEnabled = true;
  bool g_closeButtonsEnabled = true;
  float g_glass = 0.5F;
  bool g_reduceTransparency = false;

} // namespace

namespace Style {

  float cornerRadiusScale() noexcept { return g_cornerRadiusScale; }

  void setCornerRadiusScale(float scale) noexcept { g_cornerRadiusScale = std::clamp(scale, 0.0F, 2.0F); }

  bool buttonBordersEnabled() noexcept { return g_buttonBordersEnabled; }

  void setButtonBordersEnabled(bool enabled) {
    if (g_buttonBordersEnabled == enabled) {
      return;
    }
    g_buttonBordersEnabled = enabled;
    buttonBordersChanged().emit();
  }

  Signal<>& buttonBordersChanged() {
    static Signal<> signal;
    return signal;
  }

  bool inputBordersEnabled() noexcept { return g_inputBordersEnabled; }

  void setInputBordersEnabled(bool enabled) {
    if (g_inputBordersEnabled == enabled) {
      return;
    }
    g_inputBordersEnabled = enabled;
    inputBordersChanged().emit();
  }

  Signal<>& inputBordersChanged() {
    static Signal<> signal;
    return signal;
  }

  bool popupBordersEnabled() noexcept { return g_popupBordersEnabled; }
  void setPopupBordersEnabled(bool enabled) { g_popupBordersEnabled = enabled; }

  bool rtl() noexcept { return g_rtl; }
  void setRtl(bool rtl) noexcept { g_rtl = rtl; }
  bool cardBordersEnabled() noexcept { return g_cardBordersEnabled; }
  void setCardBordersEnabled(bool enabled) { g_cardBordersEnabled = enabled; }

  bool popupShadowsEnabled() noexcept { return g_popupShadowsEnabled; }
  void setPopupShadowsEnabled(bool enabled) { g_popupShadowsEnabled = enabled; }

  void setGlass(float glass, bool reduceTransparency) noexcept {
    g_glass = std::clamp(glass, 0.0F, 1.0F);
    g_reduceTransparency = reduceTransparency;
  }

  float glass() noexcept { return g_glass; }

  namespace {
    // Piecewise through Clear (0), Tinted (0.5, the design values) and Frosted (1).
    float glassLerp(float clear, float tinted, float frosted) noexcept {
      return g_glass < 0.5F ? std::lerp(clear, tinted, g_glass * 2.0F) : std::lerp(tinted, frosted, g_glass * 2.0F - 1.0F);
    }
  } // namespace

  float liquidTintAlpha(bool light) noexcept {
    return light ? glassLerp(0.20F, 0.34F, 0.84F) : glassLerp(0.24F, 0.40F, 0.60F);
  }

  float liquidTintFor(std::optional<bool> backdropDark) noexcept {
    const bool light = isLightPalette();
    const float tint = liquidTintAlpha(light);
    return backdropDark.has_value() && *backdropDark == light ? std::max(tint, 0.84F) : tint;
  }

  float liquidLevel() noexcept { return g_reduceTransparency ? 0.0F : 1.0F - g_glass; }

  float sheetLevel() noexcept { return 0.5F * liquidLevel(); }

  bool reduceTransparency() noexcept { return g_reduceTransparency; }

  float glassAlpha(float alpha) noexcept {
    if (g_reduceTransparency || alpha >= 1.0F) {
      return 1.0F;
    }
    // Configured opacities are relative to the dark tint_regular (0.70); the light scheme's tint_regular is thinner.
    const std::uint32_t tint = isLightPalette() ? Tokens::schemeLight.tintRegular : Tokens::schemeDark.tintRegular;
    const float schemeScale = static_cast<float>(tint & 0xffU) / 255.0F / kTintRegularAlpha;
    return std::min(1.0F, alpha * glassLerp(0.8F, 1.0F, 1.12F) * schemeScale);
  }

  float panelGlassAlpha(float alpha) noexcept {
    // One glass: at Tinted a light sheet has the small shapes' tint (0.88 x 0.386 = 0.34); only its blur is denser.
    const float scale = isLightPalette() ? glassLerp(0.227F, 0.386F, 1.08F) : glassLerp(0.5F, 0.72F, 1.08F);
    return g_reduceTransparency || alpha >= 1.0F ? 1.0F : std::min(1.0F, alpha * scale);
  }

  bool closeButtonsEnabled() noexcept { return g_closeButtonsEnabled; }
  void setCloseButtonsEnabled(bool enabled) { g_closeButtonsEnabled = enabled; }

  float scaledRadius(float radius, float localScale) noexcept { return radius * localScale * g_cornerRadiusScale; }

  float scaledRadiusSm(float localScale) noexcept { return scaledRadius(Tokens::radiusR8, localScale); }

  float scaledRadiusMd(float localScale) noexcept { return scaledRadius(Tokens::radiusR8, localScale); }

  float scaledRadiusLg(float localScale) noexcept { return scaledRadius(Tokens::radiusR12, localScale); }

  float scaledRadiusXl(float localScale) noexcept { return scaledRadius(Tokens::radiusR20, localScale); }

} // namespace Style
