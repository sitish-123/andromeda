#pragma once

#include "config/config_types.h"
#include "ui/app_icon_colorization.h"

#include <optional>

// The shell's [shell] app-icon keys, read for the toolkit's app-icon colorization.
[[nodiscard]] ShellAppIconColorizationSettings shellAppIconColorizationSettings(const ShellConfig& shell) noexcept;
[[nodiscard]] std::optional<ColorSpec> effectiveShellAppIconColorizationTint(const ShellConfig& shell) noexcept;
