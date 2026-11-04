#include "launcher/search_ranker.h"

#include "util/fuzzy_match.h"

#include <algorithm>
#include <cmath>

namespace {

  constexpr double kUsageScorePerCount = 0.1;
  constexpr double kTypedUsageScoreCap = 0.5;
  constexpr double kHalfLifeDays = 7.0;
  constexpr double kSecondsPerDay = 86400.0;

} // namespace

namespace search_ranker {

  double frecency(int usageCount, std::optional<std::int64_t> lastUsedSeconds, std::int64_t nowSeconds) {
    if (usageCount <= 0) {
      return 0.0;
    }
    if (!lastUsedSeconds.has_value()) {
      return usageCount;
    }
    const double days = static_cast<double>(std::max<std::int64_t>(0, nowSeconds - *lastUsedSeconds)) / kSecondsPerDay;
    return usageCount * std::pow(0.5, days / kHalfLifeDays);
  }

  double usageBoost(double score, double frecency, bool typedQuery) {
    if (frecency <= 0.0) {
      return 0.0;
    }
    const double rawBoost = frecency * kUsageScorePerCount;
    if (!typedQuery) {
      return rawBoost;
    }
    if (!FuzzyMatch::isMatch(score)) {
      return 0.0;
    }
    return std::min(rawBoost, kTypedUsageScoreCap);
  }

  void sort(std::vector<LauncherResult>& results, const std::vector<std::string>& demotedProviders) {
    const auto demoted = [&](const LauncherResult& result) {
      return std::ranges::find(demotedProviders, result.providerId) != demotedProviders.end();
    };
    std::ranges::stable_sort(results, [&](const LauncherResult& a, const LauncherResult& b) {
      const bool demotedA = demoted(a);
      const bool demotedB = demoted(b);
      if (demotedA != demotedB) {
        return demotedB;
      }
      return a.score > b.score;
    });
  }

} // namespace search_ranker
