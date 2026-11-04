#pragma once

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <cstdint>
#include <format>
#include <map>
#include <string>
#include <string_view>
#include <vector>

// Shared by the compositor (records output power and input, writes the state file) and the shell (reads it).
// Screen time per hour for Settings > Battery: "on" while the outputs are on and the user is active, "idle" while
// they are on and no input came for kIdleAfter seconds or more. Shorter input pauses count as on. Event-driven: it
// needs no timer, only the state changes.
class ScreenActivityLog {
public:
  static constexpr std::int64_t kIdleAfter = 60;
  static constexpr std::int64_t kKeepHours = 240;

  struct Hour {
    std::int64_t start = 0; // UNIX seconds, a multiple of 3600
    std::int64_t onSeconds = 0;
    std::int64_t idleSeconds = 0;
  };

  void setScreenOn(bool on, std::int64_t now);
  // The user went idle at `now` (no input since then) or became active again.
  void setUserIdle(bool idle, std::int64_t now);

  // One entry per hour in [from, to), oldest first; the running state counts up to `now`.
  [[nodiscard]] std::vector<Hour> hours(std::int64_t from, std::int64_t to, std::int64_t now) const;

  // Lines "<hour start> <on s> <idle s>"; unknown lines are skipped.
  [[nodiscard]] std::string serialize() const;
  // serialize() plus the running span up to `now`: what the compositor writes for readers.
  [[nodiscard]] std::string snapshot(std::int64_t now) const;
  void parse(std::string_view text);

private:
  void addSpan(std::map<std::int64_t, Hour>& into, std::int64_t from, std::int64_t to, bool idle) const;
  // Books [m_since, now) as on or idle time; a span inside an input pause is idle once the pause reaches kIdleAfter.
  void flush(std::int64_t now);
  [[nodiscard]] bool pauseIsIdle(std::int64_t now) const;
  void trim(std::int64_t now);

  std::map<std::int64_t, Hour> m_hours;
  bool m_screenOn = true;
  bool m_userIdle = false;
  std::int64_t m_since = -1;     // start of the span not yet in m_hours; -1 before the first event
  std::int64_t m_idleSince = 0; // start of the current input pause
};

// $XDG_STATE_HOME/andromeda/screen-activity: written by the compositor only.
inline std::string screenActivityPath() {
  const char* state = std::getenv("XDG_STATE_HOME");
  if (state != nullptr && *state == '/') {
    return std::string(state) + "/andromeda/screen-activity";
  }
  const char* home = std::getenv("HOME");
  return std::string(home != nullptr ? home : "") + "/.local/state/andromeda/screen-activity";
}

namespace screen_activity_detail {

  inline constexpr std::int64_t kHour = 3600;

  inline std::int64_t hourStart(std::int64_t t) { return t - (((t % kHour) + kHour) % kHour); }

} // namespace screen_activity_detail

inline void ScreenActivityLog::addSpan(std::map<std::int64_t, Hour>& into, std::int64_t from, std::int64_t to, bool idle)
    const {
  while (from < to) {
    const std::int64_t start = screen_activity_detail::hourStart(from);
    const std::int64_t end = std::min(to, start + screen_activity_detail::kHour);
    Hour& hour = into[start];
    hour.start = start;
    (idle ? hour.idleSeconds : hour.onSeconds) += end - from;
    from = end;
  }
}

inline bool ScreenActivityLog::pauseIsIdle(std::int64_t now) const {
  return m_userIdle && now - m_idleSince >= kIdleAfter;
}

inline void ScreenActivityLog::flush(std::int64_t now) {
  if (m_since < 0) {
    m_since = now;
  }
  if (now <= m_since) {
    return;
  }
  if (m_screenOn) {
    addSpan(m_hours, m_since, now, pauseIsIdle(now));
  }
  m_since = now;
}

inline void ScreenActivityLog::setScreenOn(bool on, std::int64_t now) {
  flush(now);
  m_screenOn = on;
  trim(now);
}

inline void ScreenActivityLog::setUserIdle(bool idle, std::int64_t now) {
  if (idle == m_userIdle) {
    return;
  }
  flush(now);
  if (idle) {
    m_idleSince = now;
  }
  m_userIdle = idle;
}

inline std::vector<ScreenActivityLog::Hour> ScreenActivityLog::hours(std::int64_t from, std::int64_t to, std::int64_t now) const {
  std::map<std::int64_t, Hour> all = m_hours;
  if (m_since >= 0 && m_screenOn && now > m_since) {
    addSpan(all, m_since, now, pauseIsIdle(now));
  }
  std::vector<Hour> out;
  for (std::int64_t start = screen_activity_detail::hourStart(from); start < to; start += screen_activity_detail::kHour) {
    const auto it = all.find(start);
    out.push_back(it != all.end() ? it->second : Hour{.start = start});
  }
  return out;
}

inline std::string ScreenActivityLog::serialize() const {
  std::string out;
  for (const auto& [start, hour] : m_hours) {
    out += std::format("{} {} {}\n", start, hour.onSeconds, hour.idleSeconds);
  }
  return out;
}

inline void ScreenActivityLog::parse(std::string_view text) {
  while (!text.empty()) {
    const std::size_t end = std::min(text.find('\n'), text.size());
    const std::string_view line = text.substr(0, end);
    text.remove_prefix(std::min(end + 1, text.size()));
    std::int64_t values[3] = {};
    const char* p = line.data();
    const char* last = line.data() + line.size();
    bool ok = true;
    for (std::int64_t& v : values) {
      while (p < last && *p == ' ') {
        ++p;
      }
      const auto [next, ec] = std::from_chars(p, last, v);
      ok = ok && ec == std::errc{} && v >= 0;
      p = next;
    }
    if (ok && p == last && values[0] % screen_activity_detail::kHour == 0 && values[1] + values[2] <= screen_activity_detail::kHour) {
      m_hours[values[0]] = Hour{.start = values[0], .onSeconds = values[1], .idleSeconds = values[2]};
    }
  }
}

inline void ScreenActivityLog::trim(std::int64_t now) {
  std::erase_if(m_hours, [now](const auto& entry) { return entry.first < screen_activity_detail::hourStart(now) - kKeepHours * screen_activity_detail::kHour; });
}

inline std::string ScreenActivityLog::snapshot(std::int64_t now) const {
  std::string out;
  for (const Hour& hour : hours(now - kKeepHours * screen_activity_detail::kHour, now + 1, now)) {
    if (hour.onSeconds > 0 || hour.idleSeconds > 0) {
      out += std::format("{} {} {}\n", hour.start, hour.onSeconds, hour.idleSeconds);
    }
  }
  return out;
}
