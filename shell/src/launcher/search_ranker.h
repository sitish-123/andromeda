#pragma once

#include "launcher/launcher_provider.h"

#include <cstdint>
#include <optional>
#include <vector>

// Pure ranking for launcher search (docs/search.md §3). No I/O and no clock: callers pass the times.
namespace search_ranker {

  // Usage count weighted by recency: count x 0.5^(days since last use / 14). An entry with no recorded use time
  // keeps its full count.
  [[nodiscard]] double frecency(int usageCount, std::optional<std::int64_t> lastUsedSeconds, std::int64_t nowSeconds);

  // Score added for a result's frecency. A typed query boosts only real matches, at most 0.5, so usage nudges close
  // matches without outranking a much stronger lexical match.
  [[nodiscard]] double usageBoost(double score, double frecency, bool typedQuery);

  // Highest score first; equal scores keep provider order. Results of demoted providers (over budget) go last.
  void sort(std::vector<LauncherResult>& results, const std::vector<std::string>& demotedProviders = {});

} // namespace search_ranker
