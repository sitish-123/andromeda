#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

// Per-hour CPU time of each app unit, of the whole machine (root cgroup), and CPU package energy (RAPL, from
// andromeda-system), for the Apps card of Settings > Battery. The compositor books the growth of each counter since
// its last read at each screen-activity write (at most every 5 min while the user is active, and at each screen on or
// off). The shell only reads appEnergyPath(): an app's energy in an hour is that hour's package energy times the
// app's share of all CPU time.
class AppEnergyLog {
public:
  static constexpr std::int64_t kBucket = 3600;
  static constexpr std::int64_t kKeepBuckets = 240;

  // `usageUsec` is the unit's cpu.stat usage_usec now. A unit seen first books all of it; a counter that went down
  // is a new unit with the same name.
  void book(const std::string& unit, std::uint64_t usageUsec, std::int64_t now) {
    const auto last = m_last.find(unit);
    const std::uint64_t grown = last == m_last.end() || last->second > usageUsec ? usageUsec : usageUsec - last->second;
    m_last[unit] = usageUsec;
    if (grown > 0) {
      m_cpu[bucket(now)][unit] += static_cast<double>(grown) / 1e6;
    }
  }

  // The root cgroup's usage_usec: all CPU time, the share's denominator.
  void bookTotal(std::uint64_t usageUsec, std::int64_t now) {
    if (m_lastTotal.has_value() && usageUsec >= *m_lastTotal) {
      m_total[bucket(now)] += static_cast<double>(usageUsec - *m_lastTotal) / 1e6;
    }
    m_lastTotal = usageUsec;
  }

  // Power.PackageEnergyJ: joules since that daemon started; a smaller value is a daemon restart and books nothing.
  void bookPackage(double joules, std::int64_t now) {
    if (std::isnan(joules)) {
      return;
    }
    if (m_lastJoules.has_value() && joules >= *m_lastJoules) {
      m_package[bucket(now)] += joules - *m_lastJoules;
    }
    m_lastJoules = joules;
  }

  // Ends the counters of units that are gone, so the file does not grow with every launch.
  template <typename Live> void keepUnits(const Live& live) {
    std::erase_if(m_last, [&live](const auto& entry) { return !live.contains(entry.first); });
  }

  // CPU seconds per unit over the last `seconds`.
  [[nodiscard]] std::map<std::string, double> cpuSeconds(std::int64_t now, std::int64_t seconds) const {
    std::map<std::string, double> out;
    for (auto it = m_cpu.lower_bound(bucket(now - seconds) + 1); it != m_cpu.end(); ++it) {
      for (const auto& [unit, s] : it->second) {
        out[unit] += s;
      }
    }
    return out;
  }

  // Wh per unit over the last `seconds`, from the hours that have package energy and all-CPU time; empty without.
  [[nodiscard]] std::map<std::string, double> wattHours(std::int64_t now, std::int64_t seconds) const {
    std::map<std::string, double> out;
    for (auto it = m_cpu.lower_bound(bucket(now - seconds) + 1); it != m_cpu.end(); ++it) {
      const auto joules = m_package.find(it->first);
      const auto total = m_total.find(it->first);
      if (joules == m_package.end() || total == m_total.end() || total->second <= 0.0) {
        continue;
      }
      for (const auto& [unit, s] : it->second) {
        out[unit] += joules->second * std::min(s / total->second, 1.0) / 3600.0;
      }
    }
    return out;
  }

  [[nodiscard]] std::string serialize(std::int64_t now) const {
    std::ostringstream out;
    const std::int64_t from = bucket(now) - kKeepBuckets + 1;
    for (auto it = m_cpu.lower_bound(from); it != m_cpu.end(); ++it) {
      for (const auto& [unit, s] : it->second) {
        out << "d " << it->first << ' ' << s << ' ' << unit << '\n';
      }
    }
    for (auto it = m_total.lower_bound(from); it != m_total.end(); ++it) {
      out << "t " << it->first << ' ' << it->second << '\n';
    }
    for (auto it = m_package.lower_bound(from); it != m_package.end(); ++it) {
      out << "e " << it->first << ' ' << it->second << '\n';
    }
    for (const auto& [unit, usage] : m_last) {
      out << "u " << usage << ' ' << unit << '\n';
    }
    if (m_lastTotal.has_value()) {
      out << "lt " << *m_lastTotal << '\n';
    }
    if (m_lastJoules.has_value()) {
      out << "le " << *m_lastJoules << '\n';
    }
    return out.str();
  }

  // Lines "d <hour> <s> <unit>", "t <hour> <s>", "e <hour> <J>", "u <usec> <unit>", "lt <usec>", "le <J>"; an hour is
  // UNIX time / 3600.
  void parse(std::string_view text) {
    std::istringstream in{std::string(text)};
    std::string kind;
    while (in >> kind) {
      std::int64_t hour = 0;
      double value = 0.0;
      std::uint64_t usage = 0;
      std::string unit;
      if (kind == "d" && in >> hour >> value >> unit) {
        m_cpu[hour][unit] += value;
      } else if (kind == "t" && in >> hour >> value) {
        m_total[hour] += value;
      } else if (kind == "e" && in >> hour >> value) {
        m_package[hour] += value;
      } else if (kind == "u" && in >> usage >> unit) {
        m_last[unit] = usage;
      } else if (kind == "lt" && in >> usage) {
        m_lastTotal = usage;
      } else if (kind == "le" && in >> value) {
        m_lastJoules = value;
      } else {
        in.clear();
        std::getline(in, unit);
      }
    }
  }

private:
  static std::int64_t bucket(std::int64_t t) { return t / kBucket; }

  std::map<std::int64_t, std::map<std::string, double>> m_cpu;
  std::map<std::int64_t, double> m_total;
  std::map<std::int64_t, double> m_package;
  std::map<std::string, std::uint64_t> m_last;
  std::optional<std::uint64_t> m_lastTotal;
  std::optional<double> m_lastJoules;
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
