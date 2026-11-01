#pragma once

#include <filesystem>
#include <string_view>

namespace greeter {

  // Greeter-only assets (Outfit font, logo): the installed share/noctalia-greeter/assets, else the source tree.
  // Toolkit assets (icon font, translations) come from paths::assetsRoot().
  [[nodiscard]] std::filesystem::path greeterAssetPath(std::string_view relativePath);

} // namespace greeter
