#include "launcher/search_ranker.h"
#include "tests/test_check.h"
#include "util/fuzzy_match.h"

#include <cstdint>
#include <string>
#include <vector>

namespace {

  constexpr std::int64_t kNow = 1'800'000'000;
  constexpr std::int64_t kDay = 86'400;

  LauncherResult result(std::string id, std::string provider, double score) {
    LauncherResult r;
    r.id = std::move(id);
    r.providerId = std::move(provider);
    r.score = score;
    return r;
  }

  std::vector<std::string> ids(const std::vector<LauncherResult>& results) {
    std::vector<std::string> out;
    for (const auto& r : results) {
      out.push_back(r.id);
    }
    return out;
  }

} // namespace

int main() {
  using namespace search_ranker;

  // Recent use outranks the same count used long ago; more uses outrank fewer at the same age.
  TEST_CHECK(frecency(5, kNow - kDay, kNow) > frecency(5, kNow - 60 * kDay, kNow));
  TEST_CHECK(frecency(6, kNow - kDay, kNow) > frecency(5, kNow - kDay, kNow));
  // Unused entries get nothing; entries from before use times were kept do not decay.
  TEST_CHECK(frecency(0, kNow, kNow) == 0.0);
  TEST_CHECK(frecency(5, std::nullopt, kNow) > frecency(5, kNow - 60 * kDay, kNow));
  // A frequent app used once long ago falls behind an app used a little, recently.
  TEST_CHECK(frecency(20, kNow - 120 * kDay, kNow) < frecency(3, kNow - kDay, kNow));

  // Typed queries: a non-match gets no boost, and the boost of a match is capped below an untyped one.
  TEST_CHECK(usageBoost(FuzzyMatch::noMatchScore, 50.0, true) == 0.0);
  TEST_CHECK(usageBoost(1.0, 50.0, true) < usageBoost(1.0, 50.0, false));
  TEST_CHECK(usageBoost(1.0, 1.0, true) > 0.0);

  // Highest score first, ties keep order, a demoted provider goes last whatever its score.
  std::vector<LauncherResult> results = {
      result("slow-best", "Slow", 9.0),
      result("a", "Apps", 2.0),
      result("b", "Apps", 3.0),
      result("c", "Windows", 2.0),
  };
  sort(results, {"Slow"});
  TEST_CHECK((ids(results) == std::vector<std::string>{"b", "a", "c", "slow-best"}));
  sort(results);
  TEST_CHECK(ids(results).front() == "slow-best");
  return 0;
}
