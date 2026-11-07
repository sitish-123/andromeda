#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

// Type-ahead select (FILES-UX D1): prefix match, case and accent insensitive. The buffer resets 1000 ms after the
// last key. A buffer of one letter repeated cycles the items that start with that letter.
class TypeAhead {
public:
  using Clock = std::chrono::steady_clock;
  static constexpr auto kReset = std::chrono::milliseconds(1000);

  // name(i) gives the display name of item i in view order; current is the cursor item.
  // Returns the item to select, or nothing (no match = no change).
  [[nodiscard]] std::optional<std::size_t> feed(
      char32_t ch, std::size_t count, const std::function<std::string_view(std::size_t)>& name,
      std::optional<std::size_t> current, Clock::time_point now = Clock::now()
  );
  // True while a key typed now would extend the buffer (Space then belongs to the name, not Quick Look).
  [[nodiscard]] bool active(Clock::time_point now = Clock::now()) const noexcept;
  void reset() noexcept { m_buffer.clear(); }

  // Lowercase, accents removed (NFKD without combining marks).
  [[nodiscard]] static std::string fold(std::string_view text);

private:
  std::u32string m_buffer;
  Clock::time_point m_last{};
};
