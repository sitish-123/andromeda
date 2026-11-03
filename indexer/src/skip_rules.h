#pragma once

#include <string_view>

namespace andromeda::index {

  // Folder names never indexed: generated or vendored trees that hold no user documents.
  [[nodiscard]] bool skipFolderName(std::string_view name);

  // Hidden entries (leading dot) are never indexed.
  [[nodiscard]] inline bool hiddenName(std::string_view name) { return name.starts_with('.'); }

  // A folder holding one of these markers is skipped with everything below it (VCS checkouts, opt-out markers).
  [[nodiscard]] bool skipMarkerName(std::string_view name);

} // namespace andromeda::index
