#pragma once

#include <charconv>
#include <optional>
#include <string_view>

// A whole-string number parse with no exceptions: nullopt for empty, partial ("12px") or out-of-range input.
template <typename T> [[nodiscard]] std::optional<T> parseNumber(std::string_view text) {
  T value{};
  const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (text.empty() || ec != std::errc{} || end != text.data() + text.size()) {
    return std::nullopt;
  }
  return value;
}
