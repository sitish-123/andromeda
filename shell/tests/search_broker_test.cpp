#include "launcher/search_broker.h"
#include "launcher/usage_tracker.h"
#include "tests/test_check.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

  using namespace std::chrono_literals;

  std::chrono::steady_clock::time_point g_now;

  class FakeProvider : public LauncherProvider {
  public:
    FakeProvider(std::string id, std::string prefix, double score) : m_id(std::move(id)), m_prefix(std::move(prefix)), m_score(score) {}

    std::string_view defaultPrefix() const override { return m_prefix; }
    std::string_view id() const override { return m_id; }
    std::optional<std::string> answeredQuery() const override { return answered; }

    std::vector<LauncherResult> query(std::string_view text) const override {
      lastText = std::string(text);
      g_now += cost;
      LauncherResult r;
      r.id = m_id + ":" + std::string(text);
      r.score = m_score;
      return {r};
    }
    bool activate(const LauncherResult& /*result*/) override { return true; }

    std::optional<std::string> answered;
    std::chrono::steady_clock::duration cost = 0ms;
    mutable std::string lastText;

  private:
    std::string m_id;
    std::string m_prefix;
    double m_score;
  };

  std::vector<std::string> providerOrder(const SearchBroker::Outcome& outcome) {
    std::vector<std::string> order;
    for (const auto& r : outcome.results) {
      order.push_back(r.providerId);
    }
    return order;
  }

} // namespace

int main() {
  // Keep the usage files out of the real state directory.
  const auto state = std::filesystem::temp_directory_path() / ("noctalia-search-broker-" + std::to_string(getpid()));
  setenv("NOCTALIA_STATE_HOME", state.c_str(), 1);

  std::vector<std::unique_ptr<LauncherProvider>> providers;
  auto* apps = new FakeProvider("Apps", "", 1.0);
  auto* plugin = new FakeProvider("Plugin", "", 2.0);
  auto* emoji = new FakeProvider("Emoji", ":", 1.0);
  providers.emplace_back(apps);
  providers.emplace_back(plugin);
  providers.emplace_back(emoji);
  UsageTracker usage;
  SearchBroker broker(providers, usage, "/", [] { return g_now; });

  // A cached async answer to an earlier query is stale: it is not shown and its callback starts no new search.
  plugin->answered = "ab";
  auto outcome = broker.search({.text = "abc"});
  TEST_CHECK((providerOrder(outcome) == std::vector<std::string>{"Apps"}));
  // The fresh answer is on its way: the launcher shows loading, not "no results".
  TEST_CHECK(outcome.loading);
  TEST_CHECK(!broker.acceptsAnswer(*plugin));
  plugin->answered = "abc";
  TEST_CHECK(broker.acceptsAnswer(*plugin));
  outcome = broker.search({.text = "abc"});
  TEST_CHECK((providerOrder(outcome) == std::vector<std::string>{"Plugin", "Apps"}));
  TEST_CHECK(!outcome.loading);
  // A new query makes the earlier answer stale.
  const auto before = broker.generation();
  (void)broker.search({.text = "abcd"});
  TEST_CHECK(broker.generation() > before);
  TEST_CHECK(!broker.acceptsAnswer(*plugin));
  plugin->answered = std::nullopt;

  // A provider over its budget three times in a row goes last, and comes back once it is fast again.
  plugin->cost = SearchBroker::kBudget + 1ms;
  for (int i = 0; i < SearchBroker::kDemoteAfterMisses - 1; ++i) {
    outcome = broker.search({.text = "x"});
    TEST_CHECK(providerOrder(outcome).front() == "Plugin");
  }
  outcome = broker.search({.text = "x"});
  TEST_CHECK((providerOrder(outcome) == std::vector<std::string>{"Apps", "Plugin"}));
  plugin->cost = 0ms;
  outcome = broker.search({.text = "x"});
  TEST_CHECK(providerOrder(outcome).front() == "Plugin");

  // A typed prefix routes to that provider only, without the prefix and its space.
  outcome = broker.search({.text = ": smile"});
  TEST_CHECK(outcome.resultProviderId == "Emoji");
  TEST_CHECK(emoji->lastText == "smile");
  TEST_CHECK((providerOrder(outcome) == std::vector<std::string>{"Emoji"}));
  // The overview prefix names no provider: the panel shows the overview.
  TEST_CHECK(broker.search({.text = "/"}).providerOverview);

  std::filesystem::remove_all(state);
  return 0;
}
