#include "system/cgroup_stats.h"

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <sstream>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

  std::string readFile(const fs::path& path) {
    std::ifstream in(path);
    std::stringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
  }

  std::uint64_t toU64(std::string_view text) {
    std::uint64_t value = 0;
    std::from_chars(text.data(), text.data() + text.size(), value);
    return value;
  }

  // Value of "key=value" or "key value" tokens inside one line.
  std::string_view field(std::string_view line, std::string_view key) {
    std::size_t pos = 0;
    while ((pos = line.find(key, pos)) != std::string_view::npos) {
      const bool start = pos == 0 || line[pos - 1] == ' ';
      const std::size_t after = pos + key.size();
      if (start && after < line.size() && (line[after] == '=' || line[after] == ' ')) {
        const std::size_t end = line.find(' ', after + 1);
        return line.substr(after + 1, end == std::string_view::npos ? std::string_view::npos : end - after - 1);
      }
      pos = after;
    }
    return {};
  }

  template <typename Fn> void forEachLine(std::string_view text, Fn&& fn) {
    while (!text.empty()) {
      const std::size_t end = text.find('\n');
      fn(text.substr(0, end));
      if (end == std::string_view::npos) {
        break;
      }
      text.remove_prefix(end + 1);
    }
  }

} // namespace

std::uint64_t parseCpuStatUsageUsec(std::string_view cpuStat) {
  std::uint64_t usage = 0;
  forEachLine(cpuStat, [&usage](std::string_view line) {
    if (line.starts_with("usage_usec ")) {
      usage = toU64(line.substr(11));
    }
  });
  return usage;
}

void parseIoStat(std::string_view ioStat, std::uint64_t& readBytes, std::uint64_t& writeBytes) {
  readBytes = 0;
  writeBytes = 0;
  forEachLine(ioStat, [&](std::string_view line) {
    readBytes += toU64(field(line, "rbytes"));
    writeBytes += toU64(field(line, "wbytes"));
  });
}

double parsePressureSomeAvg10(std::string_view pressure) {
  double value = 0.0;
  forEachLine(pressure, [&value](std::string_view line) {
    if (line.starts_with("some ")) {
      const std::string_view avg = field(line, "avg10");
      std::from_chars(avg.data(), avg.data() + avg.size(), value);
    }
  });
  return value;
}

std::string unitFromProcCgroup(std::string_view procCgroup) {
  std::string_view unit;
  forEachLine(procCgroup, [&unit](std::string_view line) {
    // cgroup v2 line: "0::/user.slice/.../app.slice/foo.service[/sub]"
    if (!line.starts_with("0::")) {
      return;
    }
    std::string_view path = line.substr(3);
    while (!path.empty()) {
      const std::size_t slash = path.find('/');
      const std::string_view part = path.substr(0, slash);
      if (part.ends_with(".service") || part.ends_with(".scope")) {
        unit = part;
      }
      if (slash == std::string_view::npos) {
        break;
      }
      path.remove_prefix(slash + 1);
    }
  });
  return std::string(unit);
}

CgroupSample readCgroupSample(const std::string& cgroupRoot, const std::string& controlGroup) {
  CgroupSample sample;
  if (controlGroup.empty()) {
    return sample;
  }
  const fs::path dir = fs::path(cgroupRoot) / controlGroup.substr(controlGroup.front() == '/' ? 1 : 0);
  const std::string cpu = readFile(dir / "cpu.stat");
  if (cpu.empty()) {
    return sample;
  }
  sample.valid = true;
  sample.cpuUsageUsec = parseCpuStatUsageUsec(cpu);
  sample.memoryBytes = toU64(readFile(dir / "memory.current"));
  parseIoStat(readFile(dir / "io.stat"), sample.ioReadBytes, sample.ioWriteBytes);
  sample.memoryPressureAvg10 = parsePressureSomeAvg10(readFile(dir / "memory.pressure"));
  sample.cpuPressureAvg10 = parsePressureSomeAvg10(readFile(dir / "cpu.pressure"));
  sample.ioPressureAvg10 = parsePressureSomeAvg10(readFile(dir / "io.pressure"));
  return sample;
}

std::string unitForPid(int pid) { return unitFromProcCgroup(readFile(std::format("/proc/{}/cgroup", pid))); }

std::vector<ProcessGroup> readProcessGroups(const std::string& cgroupRoot, const std::string& controlGroup) {
  std::map<std::string, ProcessGroup> byCommand;
  const long pageSize = ::sysconf(_SC_PAGESIZE);
  const fs::path dir = fs::path(cgroupRoot) / controlGroup.substr(!controlGroup.empty() && controlGroup.front() == '/' ? 1 : 0);
  std::vector<fs::path> cgroups{dir};
  std::error_code ec;
  for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if (it->is_directory(ec)) {
      cgroups.push_back(it->path());
    }
  }
  for (const auto& cgroup : cgroups) {
    forEachLine(readFile(cgroup / "cgroup.procs"), [&](std::string_view line) {
      if (line.empty()) {
        return;
      }
      const std::string proc = std::format("/proc/{}", line);
      std::string command = readFile(proc + "/comm");
      if (command.empty()) {
        return; // exited between the two reads
      }
      if (command.back() == '\n') {
        command.pop_back();
      }
      // statm: size resident shared ...; resident is in pages.
      const std::string statm = readFile(proc + "/statm");
      const std::size_t space = statm.find(' ');
      const std::uint64_t residentPages = space == std::string::npos ? 0 : toU64(std::string_view(statm).substr(space + 1));
      auto& group = byCommand[command];
      group.command = command;
      ++group.count;
      group.residentBytes += residentPages * static_cast<std::uint64_t>(pageSize);
    });
  }
  std::vector<ProcessGroup> groups;
  for (auto& [command, group] : byCommand) {
    groups.push_back(std::move(group));
  }
  std::ranges::sort(groups, std::greater{}, &ProcessGroup::residentBytes);
  return groups;
}

bool cgroupHasTerminal(const std::string& cgroupRoot, const std::string& controlGroup) {
  if (controlGroup.empty()) {
    return false;
  }
  const fs::path dir = fs::path(cgroupRoot) / controlGroup.substr(controlGroup.front() == '/' ? 1 : 0);
  std::vector<fs::path> cgroups{dir};
  std::error_code ec;
  for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if (it->is_directory(ec)) {
      cgroups.push_back(it->path());
    }
  }
  bool found = false;
  for (const auto& cgroup : cgroups) {
    forEachLine(readFile(cgroup / "cgroup.procs"), [&found](std::string_view pid) {
      if (found || pid.empty()) {
        return;
      }
      // stat: pid (comm) state ppid pgrp session tty_nr ...; comm may hold spaces, so count from the last ')'.
      const std::string stat = readFile(std::format("/proc/{}/stat", pid));
      const std::size_t close = stat.rfind(')');
      if (close == std::string::npos) {
        return;
      }
      std::istringstream fields(stat.substr(close + 2));
      std::string state;
      long ppid = 0;
      long pgrp = 0;
      long session = 0;
      long ttyNr = 0;
      fields >> state >> ppid >> pgrp >> session >> ttyNr;
      found = ttyNr != 0;
    });
    if (found) {
      return true;
    }
  }
  return false;
}
