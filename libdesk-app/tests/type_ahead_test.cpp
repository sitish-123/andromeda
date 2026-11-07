#include "ui/files/type_ahead.h"

#include <print>
#include <string>
#include <vector>

namespace {
  int g_failures = 0;

  void expect(bool ok, const char* what) {
    if (!ok) {
      std::println(stderr, "type_ahead_test: {}", what);
      ++g_failures;
    }
  }
} // namespace

int main() {
  const std::vector<std::string> names{"Andromeda", "budget.ods", "Résumé.pdf", "report.pdf", "roadmap.md", "zeta"};
  const auto name = [&](std::size_t i) -> std::string_view { return names[i]; };
  const auto t0 = TypeAhead::Clock::now();
  const auto ms = [&](int n) { return t0 + std::chrono::milliseconds(n); };

  TypeAhead a;
  expect(a.feed(U'r', names.size(), name, std::nullopt, ms(0)) == 2, "r selects the first r item (accent folded)");
  expect(a.feed(U'e', names.size(), name, 2, ms(100)) == 2, "re keeps Résumé");
  expect(a.feed(U'p', names.size(), name, 2, ms(200)) == 3, "rep finds report.pdf");

  TypeAhead b;
  expect(b.feed(U'r', names.size(), name, std::nullopt, ms(0)) == 2, "first r");
  expect(b.feed(U'r', names.size(), name, 2, ms(100)) == 3, "second r cycles to report");
  expect(b.feed(U'r', names.size(), name, 3, ms(200)) == 4, "third r cycles to roadmap");
  expect(b.feed(U'r', names.size(), name, 4, ms(300)) == 2, "fourth r wraps to Résumé");

  TypeAhead c;
  expect(c.feed(U'B', names.size(), name, std::nullopt, ms(0)) == 1, "case insensitive");
  expect(c.feed(U'z', names.size(), name, 1, ms(1500)) == 5, "buffer resets after 1000 ms");
  expect(!c.feed(U'q', names.size(), name, 5, ms(1600)).has_value(), "no match = no change");
  expect(c.active(ms(1700)) && !c.active(ms(2700)), "active only within the reset time");

  return g_failures == 0 ? 0 : 1;
}
