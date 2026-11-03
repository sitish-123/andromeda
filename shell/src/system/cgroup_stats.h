#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// One read of a unit's cgroup v2 files. Counters are cumulative; rates come from two samples.
struct CgroupSample {
  bool valid = false;
  std::uint64_t cpuUsageUsec = 0;
  std::uint64_t memoryBytes = 0;
  std::uint64_t ioReadBytes = 0;
  std::uint64_t ioWriteBytes = 0;
  double memoryPressureAvg10 = 0.0; // "some" share of time stalled on memory, percent
  double cpuPressureAvg10 = 0.0;
  double ioPressureAvg10 = 0.0;
};

[[nodiscard]] std::uint64_t parseCpuStatUsageUsec(std::string_view cpuStat);
// Sums rbytes and wbytes over every device line of io.stat.
void parseIoStat(std::string_view ioStat, std::uint64_t& readBytes, std::uint64_t& writeBytes);
[[nodiscard]] double parsePressureSomeAvg10(std::string_view pressure);
// The innermost .service or .scope in /proc/<pid>/cgroup, "" when there is none.
[[nodiscard]] std::string unitFromProcCgroup(std::string_view procCgroup);

// cgroupRoot is the cgroup2 mount ("/sys/fs/cgroup"); controlGroup is systemd's ControlGroup property.
[[nodiscard]] CgroupSample readCgroupSample(const std::string& cgroupRoot, const std::string& controlGroup);

// Unit name for a PID, or "" when the process is gone or not in a unit.
[[nodiscard]] std::string unitForPid(int pid);

// True when a process in the cgroup (or a child cgroup) has a controlling terminal: a shell the user attends.
[[nodiscard]] bool cgroupHasTerminal(const std::string& cgroupRoot, const std::string& controlGroup);

struct ProcessGroup {
  std::string command; // /proc/<pid>/comm
  int count = 0;
  std::uint64_t residentBytes = 0;
};

// Processes in the unit's cgroup and its child cgroups, grouped by command, largest memory first.
// Reads /proc per PID: call it for one unit on demand, never per sample for every unit.
[[nodiscard]] std::vector<ProcessGroup> readProcessGroups(const std::string& cgroupRoot, const std::string& controlGroup);
