#pragma once

#include "render/core/color.h"
#include "ui/palette.h"
#include "ui/signal.h"

#include <cstdint>
#include <optional>


struct AppIconColorizationStyle {
  Color tint = rgba(1.0F, 1.0F, 1.0F, 1.0F);
  bool monochrome = false;
};

struct ShellAppIconColorizationSettings {
  bool enabled = false;
  std::optional<ColorSpec> color;

  bool operator==(const ShellAppIconColorizationSettings&) const = default;
};

// App-icon bake: desktop/tray/taskbar/dock/launcher-panel bitmap icons (CPU desaturate + tint).

// The tint when colorization is on with no color set: a label color of the current theme.
[[nodiscard]] ColorSpec defaultAppIconColorizationTint() noexcept;
[[nodiscard]] AppIconColorizationStyle resolveAppIconColorization(const ColorSpec& tint) noexcept;
void bakeAppIconForColorization(std::uint8_t* rgba, int width, int height, const Color& tint) noexcept;

[[nodiscard]] Signal<>& shellAppIconColorizationChanged() noexcept;
void notifyShellAppIconColorizationChanged() noexcept;
