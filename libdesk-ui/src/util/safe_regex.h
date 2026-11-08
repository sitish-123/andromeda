#pragma once

#include <chrono>
#include <memory>
#include <optional>
#include <string>

using GRegex = struct _GRegex;

// A user-typed regular expression (Select by Pattern, Batch Rename) on PCRE2 through GLib: a pattern that backtracks
// without end stops at the engine's limit and at a time budget instead of freezing the window.
class SafeRegex {
public:
  [[nodiscard]] static std::optional<SafeRegex> compile(const std::string& pattern, bool caseless);

  // False for no match, a name that is not UTF-8, or a pattern past its limits (then also exhausted()).
  [[nodiscard]] bool search(const std::string& text) const;
  // ECMAScript replacement syntax ($1, $&, $$); unchanged on no match or a pattern past its limits.
  [[nodiscard]] std::string replace(const std::string& text, const std::string& replacement) const;
  // The pattern hit the backtracking limit or used up the budget: the caller says so instead of a partial result.
  [[nodiscard]] bool exhausted() const noexcept { return *m_exhausted; }

private:
  [[nodiscard]] bool overBudget() const;

  std::shared_ptr<GRegex> m_regex;
  std::shared_ptr<bool> m_exhausted = std::make_shared<bool>(false);
  std::chrono::steady_clock::time_point m_deadline;
};
