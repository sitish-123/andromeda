#pragma once

#include "launcher/launcher_provider.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class UsageTracker;

// Routes a launcher query to its providers, adds usage, ranks the results and keeps each provider in its time budget
// (docs/search.md §3). The launcher panel owns the providers; the broker borrows them.
class SearchBroker {
public:
  using Clock = std::function<std::chrono::steady_clock::time_point()>;

  struct Request {
    std::string text;
    // Only this provider answers (scoped launcher).
    std::string scopedProviderId;
    // The launcher shows this provider's own view; the text has no prefix.
    std::string providerViewId;
    // Only the applications provider answers (empty query in browser mode).
    bool applicationsOnly = false;
    bool trackUsage = true;
    bool sortByUsage = false;
  };

  struct Outcome {
    std::vector<LauncherResult> results;
    std::string resultProviderId;
    std::vector<LauncherCategory> categories;
    bool loading = false;
    bool hasRecentlyUsed = false;
    // The text starts with the overview prefix and names no provider: the panel shows the provider overview.
    bool providerOverview = false;
    // providerViewId named a provider that no longer exists.
    bool providerViewMissing = false;
  };

  // A provider over this time per query misses its budget; kDemoteAfterMisses misses in a row demote it.
  static constexpr auto kBudget = std::chrono::milliseconds(2);
  static constexpr int kDemoteAfterMisses = 3;

  SearchBroker(
      const std::vector<std::unique_ptr<LauncherProvider>>& providers, UsageTracker& usage, std::string overviewPrefix,
      Clock clock = std::chrono::steady_clock::now
  );

  void setOverviewPrefix(std::string prefix) { m_overviewPrefix = std::move(prefix); }

  // One generation per call: answers of async providers to an earlier query are stale from here on.
  [[nodiscard]] Outcome search(const Request& request);
  [[nodiscard]] std::uint64_t generation() const noexcept { return m_generation; }

  // An async provider has new results. True when they answer this generation's query (the panel searches again),
  // false when they are stale or the provider was not asked.
  [[nodiscard]] bool acceptsAnswer(const LauncherProvider& provider) const;

private:
  std::vector<LauncherResult> ask(LauncherProvider& provider, std::string_view text, bool prefixed);
  [[nodiscard]] bool isStale(const LauncherProvider& provider) const;
  [[nodiscard]] std::vector<std::string> demotedProviders() const;

  const std::vector<std::unique_ptr<LauncherProvider>>& m_providers;
  UsageTracker& m_usage;
  std::string m_overviewPrefix;
  Clock m_clock;
  std::uint64_t m_generation = 0;
  // The text each provider got in the current generation.
  std::unordered_map<const LauncherProvider*, std::string> m_asked;
  std::unordered_map<std::string, int> m_budgetMisses;
};
