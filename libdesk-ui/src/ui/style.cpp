#include "ui/style.h"

#include <algorithm>

namespace {

  float g_cornerRadiusScale = 1.0F;
  bool g_buttonBordersEnabled = true;
  bool g_inputBordersEnabled = true;
  bool g_popupBordersEnabled = true;
  bool g_rtl = false;
  bool g_popupShadowsEnabled = true;
  bool g_cardBordersEnabled = true;
  bool g_closeButtonsEnabled = true;
  float g_glassOpacity = 1.0F;
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

  void setGlass(float opacity, bool reduceTransparency) noexcept {
    g_glassOpacity = std::clamp(opacity, 0.0F, 1.0F);
    g_reduceTransparency = reduceTransparency;
  }

  bool reduceTransparency() noexcept { return g_reduceTransparency; }

  float glassAlpha(float alpha) noexcept {
    return g_reduceTransparency || alpha >= 1.0F ? 1.0F : alpha * g_glassOpacity;
  }

  bool closeButtonsEnabled() noexcept { return g_closeButtonsEnabled; }
  void setCloseButtonsEnabled(bool enabled) { g_closeButtonsEnabled = enabled; }

  float scaledRadius(float radius, float localScale) noexcept { return radius * localScale * g_cornerRadiusScale; }

  float scaledRadiusSm(float localScale) noexcept { return scaledRadius(Tokens::radiusR8, localScale); }

  float scaledRadiusMd(float localScale) noexcept { return scaledRadius(Tokens::radiusR8, localScale); }

  float scaledRadiusLg(float localScale) noexcept { return scaledRadius(Tokens::radiusR12, localScale); }

  float scaledRadiusXl(float localScale) noexcept { return scaledRadius(Tokens::radiusR20, localScale); }

} // namespace Style
