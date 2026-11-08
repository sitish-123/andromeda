#include "util/safe_regex.h"

#include <chrono>
#include <format>
#include <print>
#include <string>

namespace {
  int g_failures = 0;
  void expect(bool ok, const std::string& what) {
    if (!ok) {
      std::println(stderr, "FAIL: {}", what);
      ++g_failures;
    }
  }
} // namespace

int main() {
  // Select by Pattern and Batch Rename take regexes from the user: one that backtracks without end must not freeze.
  const auto start = std::chrono::steady_clock::now();
  const auto evil = SafeRegex::compile("^(a+)+$", false);
  expect(evil.has_value(), "the pattern compiles");
  const std::string name = std::string(40, 'a') + "!";
  int matched = 0;
  for (int i = 0; i < 2000 && evil; ++i) {
    matched += evil->search(name) ? 1 : 0;
  }
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
  expect(ms.count() < 2000, std::format("2000 names took {} ms", ms.count()));
  expect(matched == 0 && evil->exhausted(), "a runaway pattern reports itself instead of matching");

  const auto swap = SafeRegex::compile("(\\w+)-(\\d+)", false);
  expect(swap && swap->replace("IMG-042", "$2_$1") == "042_IMG", "$1 and $2 replace groups");
  expect(swap && swap->replace("IMG-042", "[$&] $$") == "[IMG-042] $", "$& is the match, $$ a dollar");
  expect(!SafeRegex::compile("(", false).has_value(), "a broken pattern does not compile");
  return g_failures == 0 ? 0 : 1;
}
