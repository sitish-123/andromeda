#include "config/app_icon_settings.h"

ShellAppIconColorizationSettings shellAppIconColorizationSettings(const ShellConfig& shell) noexcept {
  return ShellAppIconColorizationSettings{.enabled = shell.appIconColorize, .color = shell.appIconColor};
}

std::optional<ColorSpec> effectiveShellAppIconColorizationTint(const ShellConfig& shell) noexcept {
  if (!shell.appIconColorize) {
    return std::nullopt;
  }
  if (shell.appIconColor.has_value()) {
    return shell.appIconColor;
  }
  return defaultAppIconColorizationTint();
}
