#pragma once

#include <string>
#include <string_view>

namespace andromeda::index {

  // A name as search compares it: lower case without accents for Latin, Greek and Cyrillic letters (é -> e,
  // ß -> ss, Ё -> е); other text is kept as it is. A query folded the same way matches by plain byte search.
  [[nodiscard]] std::string foldName(std::string_view text);

} // namespace andromeda::index
