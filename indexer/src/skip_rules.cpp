#include "skip_rules.h"

#include <array>
#include <algorithm>

namespace andromeda::index {

  bool skipFolderName(std::string_view name) {
    static constexpr std::array<std::string_view, 6> kNames{
        "node_modules", "__pycache__", "target", "lost+found", "core-dumps", "CVS",
    };
    return std::ranges::contains(kNames, name) || name.starts_with("build-") || name == "build";
  }

  bool skipMarkerName(std::string_view name) {
    static constexpr std::array<std::string_view, 5> kMarkers{".git", ".hg", ".nomedia", ".andromedaignore", ".trackerignore"};
    return std::ranges::contains(kMarkers, name);
  }

} // namespace andromeda::index
