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
  bool g_compositorBlur = false;

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

  void setGlass(float glass, bool reduceTransparency, bool compositorBlur) noexcept {
    g_glass = std::clamp(glass, 0.0F, 1.0F);
    g_reduceTransparency = reduceTransparency;
    g_compositorBlur = compositorBlur;
  }

  bool compositorDrawsTint() noexcept { return g_compositorBlur && !g_reduceTransparency; }

  float glass() noexcept { return g_glass; }

  bool reduceTransparency() noexcept { return g_reduceTransparency; }

  glass::Material glassMaterial(glass::Kind kind) noexcept {
    return glass::material(kind, g_glass, g_reduceTransparency, isLightPalette());
  }

  float glassTint(glass::Kind kind, float opacity) noexcept {
    return glass::tintFor(glassMaterial(kind), kind, isLightPalette(), opacity);
  }

  bool closeButtonsEnabled() noexcept { return g_closeButtonsEnabled; }
  void setCloseButtonsEnabled(bool enabled) { g_closeButtonsEnabled = enabled; }

  float scaledRadius(float radius, float localScale) noexcept { return radius * localScale * g_cornerRadiusScale; }

  float scaledRadiusSm(float localScale) noexcept { return scaledRadius(Tokens::radiusR8, localScale); }

  float scaledRadiusMd(float localScale) noexcept { return scaledRadius(Tokens::radiusR8, localScale); }

  float scaledRadiusLg(float localScale) noexcept { return scaledRadius(Tokens::radiusR12, localScale); }

  float scaledRadiusXl(float localScale) noexcept { return scaledRadius(Tokens::radiusR20, localScale); }

} // namespace Style
