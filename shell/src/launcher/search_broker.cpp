#include "launcher/search_broker.h"

#include "core/log.h"
#include "launcher/search_ranker.h"
#include "launcher/usage_tracker.h"
#include "util/string_utils.h"

#include <algorithm>

namespace {

  constexpr Logger kLog("search");
  // Prefixed providers that opt into global search answer only from this many typed characters on, so an empty or
  // short query does not flood the launcher with their entries.
  constexpr std::size_t kGlobalOptInMinChars = 2;
  constexpr std::string_view kApplicationsProviderId = "Applications";

  std::int64_t nowSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
  }

} // namespace

SearchBroker::SearchBroker(
    const std::vector<std::unique_ptr<LauncherProvider>>& providers, UsageTracker& usage, std::string overviewPrefix,
    Clock clock
)
    : m_providers(providers), m_usage(usage), m_overviewPrefix(std::move(overviewPrefix)), m_clock(std::move(clock)) {}

std::vector<LauncherResult> SearchBroker::ask(LauncherProvider& provider, std::string_view text, bool prefixed) {
  m_asked[&provider] = std::string(text);
  const auto start = m_clock();
  auto results = prefixed ? provider.queryPrefixed(text) : provider.query(text);
  const auto elapsed = m_clock() - start;

  const std::string id(provider.id());
  if (elapsed > kBudget) {
    const int misses = ++m_budgetMisses[id];
    const double ms = std::chrono::duration<double, std::milli>(elapsed).count();
    if (misses == kDemoteAfterMisses) {
      kLog.info("provider {} took {:.1f} ms, over its {} ms budget {} times: its results go last", id, ms,
                kBudget.count(), misses);
    } else {
      kLog.debug("provider {} took {:.1f} ms (budget {} ms, miss {})", id, ms, kBudget.count(), misses);
    }
  } else {
    m_budgetMisses.erase(id);
  }

  if (isStale(provider)) {
    results.clear();
  }
  for (auto& result : results) {
    result.providerId = id;
  }
  return results;
}

bool SearchBroker::isStale(const LauncherProvider& provider) const {
  const auto answered = provider.answeredQuery();
  if (!answered.has_value()) {
    return false;
  }
  const auto asked = m_asked.find(&provider);
  return asked == m_asked.end() || asked->second != *answered;
}

bool SearchBroker::acceptsAnswer(const LauncherProvider& provider) const {
  return m_asked.contains(&provider) && !isStale(provider);
}

std::vector<std::string> SearchBroker::demotedProviders() const {
  std::vector<std::string> demoted;
  for (const auto& [id, misses] : m_budgetMisses) {
    if (misses >= kDemoteAfterMisses) {
      demoted.push_back(id);
    }
  }
  return demoted;
}

SearchBroker::Outcome SearchBroker::search(const Request& request) {
  ++m_generation;
  m_asked.clear();
  Outcome outcome;
  const std::int64_t now = nowSeconds();

  const auto addUsage = [&](std::vector<LauncherResult>& results, const LauncherProvider& provider, bool typedQuery) {
    if (!request.trackUsage || !provider.trackUsage()) {
      return;
    }
    for (auto& result : results) {
      if (request.sortByUsage) {
        const double frecency = search_ranker::frecency(
            m_usage.getCount(provider.id(), result.id), m_usage.getLastUsed(provider.id(), result.id), now
        );
        result.score += search_ranker::usageBoost(result.score, frecency, typedQuery);
      }
      result.recentlyUsedIndex = m_usage.getRecentlyUsedIndex(provider.id(), result.id);
    }
    if (m_usage.getRecentlyUsedCount(provider.id()) > 0) {
      outcome.hasRecentlyUsed = true;
    }
  };

  if (!request.scopedProviderId.empty()) {
    outcome.resultProviderId = request.scopedProviderId;
    for (const auto& provider : m_providers) {
      if (provider->id() == request.scopedProviderId) {
        outcome.results = ask(*provider, request.text, false);
        outcome.loading = provider->isLoading() || isStale(*provider);
        break;
      }
    }
    search_ranker::sort(outcome.results);
    return outcome;
  }

  LauncherProvider* active = nullptr;
  std::string_view text = request.text;
  if (!request.providerViewId.empty()) {
    const auto provider = std::ranges::find(m_providers, std::string_view(request.providerViewId), [](const auto& p) {
      return p->id();
    });
    if (provider != m_providers.end()) {
      active = provider->get();
    } else {
      outcome.providerViewMissing = true;
    }
  }
  if (active == nullptr) {
    // The longest matching prefix wins.
    for (const auto& provider : m_providers) {
      const auto prefix = provider->prefix();
      if (!prefix.empty()
          && std::string_view(request.text).starts_with(prefix)
          && (active == nullptr || prefix.size() > active->prefix().size())) {
        active = provider.get();
        text = std::string_view(request.text).substr(prefix.size());
      }
    }
  }
  if (active != nullptr && text.starts_with(' ')) {
    text.remove_prefix(1);
  }
  const bool typedQuery = !text.empty();

  if (active != nullptr) {
    outcome.resultProviderId = active->id();
    outcome.results = ask(*active, text, true);
    outcome.loading = active->isLoading() || isStale(*active);
    addUsage(outcome.results, *active, typedQuery);
    search_ranker::sort(outcome.results);
    outcome.categories = active->categories();
    return outcome;
  }

  if (!m_overviewPrefix.empty() && std::string_view(request.text).starts_with(m_overviewPrefix)) {
    outcome.providerOverview = true;
    return outcome;
  }

  const bool allowGlobalOptIn = StringUtils::trimRightView(StringUtils::trimLeftView(text)).size() >= kGlobalOptInMinChars;
  for (const auto& provider : m_providers) {
    if (request.applicationsOnly && provider->id() != kApplicationsProviderId) {
      continue;
    }
    const bool isDefault = provider->prefix().empty();
    if (!isDefault && (!provider->includeInGlobalSearch() || !allowGlobalOptIn)) {
      continue;
    }
    auto results = ask(*provider, text, false);
    outcome.loading = outcome.loading || provider->isLoading() || isStale(*provider);
    addUsage(results, *provider, typedQuery);
    outcome.results.insert(
        outcome.results.end(), std::make_move_iterator(results.begin()), std::make_move_iterator(results.end())
    );
    auto categories = provider->categories();
    outcome.categories.insert(
        outcome.categories.end(), std::make_move_iterator(categories.begin()), std::make_move_iterator(categories.end())
    );
  }
  search_ranker::sort(outcome.results, demotedProviders());
  return outcome;
}
