#include "util/fuzzy_match.h"

#include "match.h"
#include "util/string_utils.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>
#include <vector>

namespace FuzzyMatch {

  double score(std::string_view pattern, std::string_view text) {
    if (pattern.empty()) {
      return 0.0;
    }
    if (text.empty()) {
      return noMatchScore;
    }

    // fzy's has_match only widens lowercase needle chars to match either case, so an
    // uppercase query char would fail to match lowercase text. Lowercase the needle to
    // keep matching fully case-insensitive (the scorer lowercases the haystack itself).
    const std::string needle = StringUtils::toLower(pattern);
    const std::string haystack(text);
    if (has_match(needle.c_str(), haystack.c_str()) == 0) {
      return noMatchScore;
    }

    const score_t rawScore = match(needle.c_str(), haystack.c_str());
    if (rawScore == SCORE_MAX) {
      return 1024.0;
    }
    if (rawScore == SCORE_MIN || !std::isfinite(rawScore)) {
      return noMatchScore;
    }
    return rawScore;
  }

  std::optional<int> typoDistance(std::string_view pattern, std::string_view text) {
    const int allowed = pattern.size() < 4 ? 0 : (pattern.size() < 7 ? 1 : 2);
    if (allowed == 0) {
      return std::nullopt;
    }
    const auto fold = [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); };
    const std::size_t m = pattern.size();
    std::optional<int> best;

    for (std::size_t start = 0; start < text.size(); ++start) {
      const bool wordStart = start == 0 || std::isalnum(static_cast<unsigned char>(text[start - 1])) == 0;
      if (!wordStart || std::isalnum(static_cast<unsigned char>(text[start])) == 0) {
        continue;
      }
      const std::string_view word = text.substr(start, std::min(text.size() - start, m + static_cast<std::size_t>(allowed)));
      const std::size_t n = word.size();
      // Optimal string alignment table; the answer is the best prefix of the word, so the last row's minimum.
      std::vector<int> d((m + 1) * (n + 1));
      const auto at = [&](std::size_t i, std::size_t j) -> int& { return d[i * (n + 1) + j]; };
      for (std::size_t i = 0; i <= m; ++i) {
        at(i, 0) = static_cast<int>(i);
      }
      for (std::size_t j = 0; j <= n; ++j) {
        at(0, j) = static_cast<int>(j);
      }
      for (std::size_t i = 1; i <= m; ++i) {
        for (std::size_t j = 1; j <= n; ++j) {
          const int cost = fold(pattern[i - 1]) == fold(word[j - 1]) ? 0 : 1;
          at(i, j) = std::min({at(i - 1, j) + 1, at(i, j - 1) + 1, at(i - 1, j - 1) + cost});
          if (i > 1 && j > 1 && fold(pattern[i - 1]) == fold(word[j - 2]) && fold(pattern[i - 2]) == fold(word[j - 1])) {
            at(i, j) = std::min(at(i, j), at(i - 2, j - 2) + 1);
          }
        }
      }
      int distance = at(m, 0);
      for (std::size_t j = 1; j <= n; ++j) {
        distance = std::min(distance, at(m, j));
      }
      if (distance <= allowed && (!best.has_value() || distance < *best)) {
        best = distance;
      }
    }
    return best;
  }

} // namespace FuzzyMatch
