#pragma once

#include <limits>
#include <optional>
#include <string_view>

namespace FuzzyMatch {

  inline constexpr double noMatchScore = -std::numeric_limits<double>::infinity();

  [[nodiscard]] inline bool isMatch(double value) noexcept { return value > noMatchScore; }

  [[nodiscard]] double score(std::string_view pattern, std::string_view text);

  // Fewest edits (insert, delete, change, swap of neighbours) that turn the pattern into the start of the text or of a
  // word in it, ASCII case folded; nullopt above the allowance: no typo under 4 letters, 1 from 4, 2 from 7. After
  // nanolight (github.com/justrach/nanolight, MIT): "chorme" finds Chrome.
  [[nodiscard]] std::optional<int> typoDistance(std::string_view pattern, std::string_view text);

} // namespace FuzzyMatch
