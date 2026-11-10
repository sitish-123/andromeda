#pragma once

#include <string>
#include <string_view>

// Compile shim for UI callers; removed in D2b.
namespace compositors::hyprland {

  [[nodiscard]] inline std::string normalizeWindowId(std::string_view /*value*/) { return {}; }

  [[nodiscard]] inline bool windowIdsEqual(std::string_view /*lhs*/, std::string_view /*rhs*/) { return false; }

} // namespace compositors::hyprland
