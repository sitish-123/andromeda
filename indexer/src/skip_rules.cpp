#include "skip_rules.h"

#include <array>
#include <algorithm>

namespace andromeda::index {

  bool skipFolderName(std::string_view name) {
    static constexpr std::array<std::string_view, 5> kNames{"node_modules", "__pycache__", "lost+found", "core-dumps", "CVS"};
    return std::ranges::contains(kNames, name);
  }

  bool skipMarkerName(std::string_view name) {
    // Build output is found by what it holds, not by name: "build-tools" or a "target" folder can be documents.
    // CACHEDIR.TAG marks Cargo and Meson output and caches; build.ninja and CMakeCache.txt mark Ninja and CMake trees.
    static constexpr std::array<std::string_view, 8> kMarkers{
        ".git", ".hg", ".nomedia", ".andromedaignore", ".trackerignore", "CACHEDIR.TAG", "build.ninja", "CMakeCache.txt",
    };
    return std::ranges::contains(kMarkers, name);
  }

} // namespace andromeda::index
