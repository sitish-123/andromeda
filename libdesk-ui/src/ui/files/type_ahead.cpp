#include "ui/files/type_ahead.h"

#include <glib.h>

std::string TypeAhead::fold(std::string_view text) {
  gchar* decomposed = g_utf8_normalize(text.data(), static_cast<gssize>(text.size()), G_NORMALIZE_NFKD);
  if (decomposed == nullptr) {
    return std::string(text);
  }
  std::string out;
  for (const gchar* p = decomposed; *p != '\0'; p = g_utf8_next_char(p)) {
    const gunichar c = g_utf8_get_char(p);
    if (g_unichar_ismark(c)) {
      continue;
    }
    gchar buf[6];
    out.append(buf, static_cast<std::size_t>(g_unichar_to_utf8(g_unichar_tolower(c), buf)));
  }
  g_free(decomposed);
  return out;
}

bool TypeAhead::active(Clock::time_point now) const noexcept { return !m_buffer.empty() && now - m_last < kReset; }

std::optional<std::size_t> TypeAhead::feed(
    char32_t ch, std::size_t count, const std::function<std::string_view(std::size_t)>& name,
    std::optional<std::size_t> current, Clock::time_point now
) {
  if (!active(now)) {
    m_buffer.clear();
  }
  m_last = now;
  m_buffer.push_back(ch);
  if (count == 0) {
    return std::nullopt;
  }

  const bool repeated = m_buffer.size() > 1 && m_buffer.find_first_not_of(m_buffer.front()) == std::u32string::npos;
  std::u32string needle32 = repeated ? m_buffer.substr(0, 1) : m_buffer;
  std::string utf8;
  for (const char32_t c : needle32) {
    gchar buf[6];
    utf8.append(buf, static_cast<std::size_t>(g_unichar_to_utf8(static_cast<gunichar>(c), buf)));
  }
  const std::string needle = fold(utf8);

  // A new letter keeps the cursor item when it still matches; a repeated letter moves on to the next match.
  const std::size_t start = current.has_value() && *current < count ? *current + (repeated || m_buffer.size() == 1 ? 1 : 0) : 0;
  for (std::size_t step = 0; step < count; ++step) {
    const std::size_t i = (start + step) % count;
    if (fold(name(i)).starts_with(needle)) {
      return i;
    }
  }
  return std::nullopt;
}
