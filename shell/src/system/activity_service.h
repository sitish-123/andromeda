#pragma once

#include "dbus/systemd/systemd_unit_name.h"
#include "dbus/systemd/systemd_units_service.h"
#include "system/cgroup_stats.h"

#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <vector>

// One row of the Activity app: every running unit with the same application ID, or one unit that has none.
struct ActivityRow {
  std::string key;   // application ID, else the unit name
  std::string appId; // from the unit name, else from a window in the unit
  std::vector<std::string> units;
  bool hasWindow = false;
  bool systemManager = false;
  UnitSlice slice = UnitSlice::Other;
  std::uint64_t startedUsec = 0; // earliest ActiveEnterTimestamp, CLOCK_REALTIME
  // Rates over the last kRateWindow; 0 until two samples exist.
  double cpuPercent = 0.0; // share of all CPUs
  std::uint64_t memoryBytes = 0;
  std::uint64_t cpuUsageUsec = 0; // cumulative, for averages over the unit's lifetime
  double ioReadBytesPerSec = 0.0;
  double ioWriteBytesPerSec = 0.0;
  double memoryPressure = 0.0; // highest avg10 over the row's units, percent
};

class ActivityService {
public:
  static constexpr auto kRateWindow = std::chrono::seconds(5);

  explicit ActivityService(std::string cgroupRoot = "/sys/fs/cgroup", int cpuCount = 0);

  // Running units from every manager; replaces the previous set.
  void setUnits(std::vector<SystemdUnit> units);
  // Unit name -> app_id of a window whose PID is in that unit (see unitForPid()).
  void setWindowUnits(std::map<std::string, std::string> windowAppIdByUnit);
  // Reads every unit's cgroup once and rebuilds rows().
  void sample(std::chrono::steady_clock::time_point now);

  [[nodiscard]] const std::string& cgroupRoot() const noexcept { return m_cgroupRoot; }
  [[nodiscard]] int cpuCount() const noexcept { return m_cpuCount; }
  // Sorted by memory, largest first.
  [[nodiscard]] const std::vector<ActivityRow>& rows() const noexcept { return m_rows; }

private:
  struct Timed {
    std::chrono::steady_clock::time_point at;
    CgroupSample sample;
  };

  void rebuild();

  std::string m_cgroupRoot;
  int m_cpuCount;
  std::vector<SystemdUnit> m_units;
  std::map<std::string, std::string> m_windowAppIdByUnit;
  std::map<std::string, std::deque<Timed>> m_history; // by unit name, oldest first
  std::vector<ActivityRow> m_rows;
};
