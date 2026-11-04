#pragma once

#include <cstdint>
#include <cstdlib>
#include <map>
#include <sstream>
#include <string>
#include <string_view>

// CPU time of each app unit by day, for the 10-day Apps view of Settings > Battery. The compositor reads each app
// unit's cpu.stat when it writes screen activity (at most every 5 min while the user is active, and at each screen
// on or off) and books the growth since its last read to that day. The shell only reads appEnergyPath() and maps
// units to apps.
class AppEnergyLog {
public:
  static constexpr std::int64_t kDay = 86400;
  static constexpr std::int64_t kKeepDays = 10;

  // `usageUsec` is the unit's cpu.stat usage_usec now. A unit seen first books all of it; a counter that went down
  // is a new unit with the same name.
  void book(const std::string& unit, std::uint64_t usageUsec, std::int64_t now) {
    const auto last = m_last.find(unit);
    const std::uint64_t grown = last == m_last.end() || last->second > usageUsec ? usageUsec : usageUsec - last->second;
    m_last[unit] = usageUsec;
    if (grown > 0) {
      m_days[day(now)][unit] += static_cast<double>(grown) / 1e6;
    }
  }

  // Ends the counters of units that are gone, so the file does not grow with every launch.
  template <typename Live> void keepUnits(const Live& live) {
    std::erase_if(m_last, [&live](const auto& entry) { return !live.contains(entry.first); });
  }

  // CPU seconds per unit over the last `days` days, today included.
  [[nodiscard]] std::map<std::string, double> totals(std::int64_t now, std::int64_t days) const {
    std::map<std::string, double> out;
    for (auto it = m_days.lower_bound(day(now) - days + 1); it != m_days.end(); ++it) {
      for (const auto& [unit, seconds] : it->second) {
        out[unit] += seconds;
      }
    }
    return out;
  }

  // Lines "d <day> <seconds> <unit>" and "u <usage_usec> <unit>"; days older than kKeepDays are left out.
  [[nodiscard]] std::string serialize(std::int64_t now) const {
    std::ostringstream out;
    for (auto it = m_days.lower_bound(day(now) - kKeepDays + 1); it != m_days.end(); ++it) {
      for (const auto& [unit, seconds] : it->second) {
        out << "d " << it->first << ' ' << seconds << ' ' << unit << '\n';
      }
    }
    for (const auto& [unit, usage] : m_last) {
      out << "u " << usage << ' ' << unit << '\n';
    }
    return out.str();
  }

  void parse(std::string_view text) {
    std::istringstream in{std::string(text)};
    std::string kind;
    while (in >> kind) {
      std::string unit;
      if (kind == "d") {
        std::int64_t d = 0;
        double seconds = 0.0;
        if (in >> d >> seconds >> unit) {
          m_days[d][unit] += seconds;
        }
      } else if (kind == "u") {
        std::uint64_t usage = 0;
        if (in >> usage >> unit) {
          m_last[unit] = usage;
        }
      } else {
        std::getline(in, unit);
      }
    }
  }

private:
  static std::int64_t day(std::int64_t t) { return t / kDay; }

  std::map<std::int64_t, std::map<std::string, double>> m_days;
  std::map<std::string, std::uint64_t> m_last;
};

// $XDG_STATE_HOME/andromeda/app-energy: written by the compositor only.
inline std::string appEnergyPath() {
  const char* state = std::getenv("XDG_STATE_HOME");
  if (state != nullptr && *state == '/') {
    return std::string(state) + "/andromeda/app-energy";
  }
  const char* home = std::getenv("HOME");
  return std::string(home != nullptr ? home : "") + "/.local/state/andromeda/app-energy";
}
