#pragma once

#include "config/config_service.h"
#include "config/config_types.h"
#include "ui/surface_shadow.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <cstdint>

// Sizing and styling shared by the panel surfaces: PanelManager's single active
// panel and PersistentPanelHost's long-lived ones.
namespace shell::panel_surface {

  // Extra margin around the shadow bleed. The shadow is drawn inside the surface,
  // so a surface sized to the bleed alone clips its own outermost falloff row.
  inline constexpr std::int32_t kShadowSafetyPadding = 2;

  [[nodiscard]] inline ui::surface_shadow::Bleed
  bleed(bool hasDecoration, const ShellConfig::ShadowConfig& shadow) noexcept {
    auto out = ui::surface_shadow::bleed(hasDecoration, shadow);
    if (ui::surface_shadow::enabled(hasDecoration, shadow)) {
      out.left += kShadowSafetyPadding;
      out.right += kShadowSafetyPadding;
      out.up += kShadowSafetyPadding;
      out.down += kShadowSafetyPadding;
    }
    return out;
  }

  [[nodiscard]] inline std::uint32_t
  surfaceExtent(std::uint32_t contentSize, std::int32_t before, std::int32_t after) noexcept {
    const auto total =
        static_cast<std::int64_t>(contentSize) + static_cast<std::int64_t>(before) + static_cast<std::int64_t>(after);
    return static_cast<std::uint32_t>(std::max<std::int64_t>(1, total));
  }

  [[nodiscard]] inline float contentScale(const ConfigService* configService) noexcept {
    if (configService == nullptr) {
      return 1.0F;
    }
    return std::max(0.1F, configService->config().accessibility.uiScale);
  }

  [[nodiscard]] inline float backgroundOpacity(const ConfigService* configService) noexcept {
    const auto mode =
        configService != nullptr ? configService->config().shell.panel.transparencyMode : PanelTransparencyMode::Solid;
    return Style::glassTint(glass::Kind::Sheet, detachedPanelBackgroundOpacityForTransparencyMode(mode));
  }

  [[nodiscard]] inline ColorSpec
  borderColor(const ConfigService* configService, float panelBackgroundOpacity) noexcept {
    const ColorSpec spec = configService != nullptr ? configService->config().shell.panel.borderColor
                                                    : colorSpecFromRole(ColorRole::Stroke);
    return scaleAlpha(spec, panelBackgroundOpacity);
  }

  [[nodiscard]] inline float borderWidth(const ConfigService* configService) noexcept {
    if (configService == nullptr) {
      return Style::borderWidth;
    }
    return std::max(0.0F, configService->config().shell.panel.borderWidth);
  }

  // Cards draw their translucent fill token as is; the compositor tint under them follows the glass slider.
  [[nodiscard]] inline float cardOpacity(const ConfigService* /*configService*/, float /*panelBackgroundOpacity*/) noexcept {
    return 1.0F;
  }

} // namespace shell::panel_surface
