#pragma once

#include "design_tokens.h"
#include "motion/desk_motion.h"
#include "ui/signal.h"

namespace Style {

  [[nodiscard]] inline desk::motion::SpringConfig spring(const Tokens::Spring& token) {
    return {.damping = token.damping, .stiffness = token.stiffness};
  }

  TOKENS_LIVE(barThicknessDefault, Tokens::componentIslandHeight);

  inline constexpr float borderWidth = 1.0F;
  inline constexpr float emphasizedBorderWidth = 3.0F;
  inline constexpr float focusRingWidth = 2.0F;
  inline constexpr float focusRingGap = 2.0F;
  inline constexpr float progressMarkerWidth = 2.0F;
  inline constexpr float disabledOutlineAlpha = 0.5F;

  inline constexpr const float& spaceXs = Tokens::spaceP4;
  inline constexpr const float& spaceSm = Tokens::spaceP8;
  inline constexpr const float& spaceMd = Tokens::spaceP12;
  inline constexpr const float& spaceLg = Tokens::spaceP16;

  inline constexpr const float& cardPadding = Tokens::spaceP12;
  inline constexpr const float& panelPadding = Tokens::spaceP12;

  // Default inner inset for bar widget capsules (logical px, before bar content scale): R5, symbol end and text end.
  inline constexpr const float& barCapsuleInsetSymbol = Tokens::componentIslandInsetSymbol;
  inline constexpr const float& barCapsuleInsetText = Tokens::componentIslandInsetText;
  inline constexpr float baseGlyphSize = 16.0F;

  inline constexpr const float& fontSizeMini = Tokens::typeCaption.size;
  inline constexpr const float& fontSizeCaption = Tokens::typeFootnote.size;
  inline constexpr const float& fontSizeBody = Tokens::typeBody.size;
  inline constexpr const float& fontSizeTitle = Tokens::typeHeadline.size;
  inline constexpr const float& fontSizeHeader = Tokens::typeTitle1.size;

  inline constexpr const float& controlHeightSm = Tokens::sizeControlS;
  inline constexpr const float& controlHeight = Tokens::sizeControlM;
  inline constexpr const float& controlHeightLg = Tokens::sizeControlL;
  // Body and footnote text on glass is one step heavier (400 -> 500); other weights stay.
  [[nodiscard]] inline int glassWeight(int weight) noexcept {
    return weight == 400 ? weight + Tokens::typeRulesGlassWeightAdd : weight;
  }
  inline constexpr float scrollWheelStep = 56.0F;
  // Pointer distance in logical px before an armed drag becomes active.
  inline constexpr const float& dragStartThreshold = Tokens::motionSlop;

  // Base scrollbar thickness at rest, and while the pointer is over it. The bar overlays the
  // content as it expands, so hovering never reflows the scroll view.
  inline constexpr const float& scrollbarWidth = Tokens::componentScrollbarWidth;
  inline constexpr const float& scrollbarHoverWidth = Tokens::componentScrollbarHoverWidth;
  inline constexpr const float& scrollbarGap = Tokens::componentScrollbarInset;
  // Shortest the thumb gets on a long document; it must stay a usable drag target.
  inline constexpr const float& scrollbarMinThumbHeight = Tokens::componentScrollbarMinLength;
  // Pointer margin on the content side of the bar, so a thin bar stays an easy grab target.
  inline constexpr const float& scrollbarHitSlop = Tokens::motionSlop;

  // Growth cap (logical px, before content scale) for menus/dropdowns that size to their content.
  inline constexpr const float& menuAutoMaxWidth = Tokens::componentMenuMaxWidth;

  // Window switcher carousel geometry and depth treatment.
  inline constexpr float windowSwitcherDimOpacity = 0.3F;
  inline constexpr float windowSwitcherSelectedCardWidth = 500.0F;
  inline constexpr float windowSwitcherNearCardWidth = 380.0F;
  inline constexpr float windowSwitcherFarCardWidth = 280.0F;
  inline constexpr float windowSwitcherCompactCardWidth = 220.0F;
  inline constexpr float windowSwitcherPreviewAspect = 1.6F;
  TOKENS_LIVE(windowSwitcherCardOverlap, controlHeightLg + spaceLg);
  inline constexpr float windowSwitcherNarrowLayoutThreshold = 0.72F;
  inline constexpr float windowSwitcherPreviewIconScale = 0.12F;
  inline constexpr float windowSwitcherFallbackIconScale = 0.28F;
  TOKENS_LIVE(windowSwitcherCaptionHeight, controlHeightLg);
  TOKENS_LIVE(windowSwitcherCaptionLineGap, -spaceXs);
  inline constexpr float windowSwitcherIncomingCardScale = 0.9F;
  inline constexpr float windowSwitcherIncomingCardSlide = 0.25F;
  inline constexpr float windowSwitcherOutgoingCardSlide = 0.3F;
  inline constexpr float windowSwitcherRevealScale = 0.95F;

  // One toggle size for every preset. Track height = thumb + 2 * inset; track width = thumb + 2 * inset + travel.
  inline constexpr const float& toggleThumbSizeSm = Tokens::componentToggleKnob;
  inline constexpr const float& toggleInsetSm = Tokens::componentToggleInset;
  TOKENS_LIVE(toggleTravelSm, Tokens::componentToggleWidth - Tokens::componentToggleKnob - 2.0F * Tokens::componentToggleInset);
  inline constexpr const float& toggleThumbSizeMd = Tokens::componentToggleKnob;
  inline constexpr const float& toggleInsetMd = Tokens::componentToggleInset;
  TOKENS_LIVE(toggleTravelMd, toggleTravelSm);
  inline constexpr const float& toggleThumbSizeLg = Tokens::componentToggleKnob;
  inline constexpr const float& toggleInsetLg = Tokens::componentToggleInset;
  TOKENS_LIVE(toggleTravelLg, toggleTravelSm);

  // Slider geometry.
  inline constexpr float sliderDefaultWidth = 180.0F;
  inline constexpr const float& sliderTrackHeight = Tokens::componentRangeTrack;
  inline constexpr const float& sliderThumbSize = Tokens::componentRangeKnob;
  inline constexpr float sliderHorizontalPadding = 2.0F;

  [[nodiscard]] float cornerRadiusScale() noexcept;
  void setCornerRadiusScale(float scale) noexcept;

  [[nodiscard]] bool buttonBordersEnabled() noexcept;
  void setButtonBordersEnabled(bool enabled);
  Signal<>& buttonBordersChanged();

  [[nodiscard]] bool inputBordersEnabled() noexcept;
  void setInputBordersEnabled(bool enabled);
  Signal<>& inputBordersChanged();

  [[nodiscard]] bool popupBordersEnabled() noexcept;
  void setPopupBordersEnabled(bool enabled);

  [[nodiscard]] bool rtl() noexcept;
  void setRtl(bool rtl) noexcept;

  [[nodiscard]] bool cardBordersEnabled() noexcept;
  void setCardBordersEnabled(bool enabled);

  [[nodiscard]] bool popupShadowsEnabled() noexcept;
  void setPopupShadowsEnabled(bool enabled);

  // Live appearance for glass materials: `opacity` (0..1) multiplies their alpha; reduce transparency makes them solid.
  // glass: the Clear (0) to Frosted (1) slider, 0.5 = Tinted, the design values (appearance.glass).
  void setGlass(float glass, bool reduceTransparency) noexcept;
  [[nodiscard]] float glass() noexcept;
  // Alpha a glass material fill uses in place of its configured `alpha`.
  [[nodiscard]] float glassAlpha(float alpha) noexcept;
  // glassAlpha without the light scheme's thinner tint, so panel sheets stay dense over dark content.
  [[nodiscard]] float panelGlassAlpha(float alpha) noexcept;
  // Tint of liquid glass shapes (islands, dock, launcher pill) at the glass level; Tinted: light 0.62, dark 0.40.
  [[nodiscard]] float liquidTintAlpha(bool light) noexcept;
  // dsk_surface_v1 liquid value [0] for a liquid shape: 1 at Clear, 0 at Frosted, 0 with Reduce Transparency.
  [[nodiscard]] float liquidLevel() noexcept;
  // The liquid value for sheets: half the shapes' clarity, so their blur is a step denser, not a different material.
  [[nodiscard]] float sheetLevel() noexcept;
  [[nodiscard]] bool reduceTransparency() noexcept;

  [[nodiscard]] bool closeButtonsEnabled() noexcept;
  void setCloseButtonsEnabled(bool enabled);

  [[nodiscard]] float scaledRadius(float radius, float localScale = 1.0F) noexcept;
  [[nodiscard]] float scaledRadiusSm(float localScale = 1.0F) noexcept;
  [[nodiscard]] float scaledRadiusMd(float localScale = 1.0F) noexcept;
  [[nodiscard]] float scaledRadiusLg(float localScale = 1.0F) noexcept;
  [[nodiscard]] float scaledRadiusXl(float localScale = 1.0F) noexcept;

} // namespace Style
