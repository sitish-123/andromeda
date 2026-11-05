#include "qos/cpu_topology.h"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <system_error>

namespace andromeda::qos {

  namespace {

    std::optional<long long> readNumber(const std::filesystem::path& path) {
      std::ifstream in(path);
      long long value = 0;
      if (!(in >> value)) {
        return std::nullopt;
      }
      return value;
    }

    std::string readLine(const std::filesystem::path& path) {
      std::ifstream in(path);
      std::string line;
      std::getline(in, line);
      return line;
    }

    std::optional<int> cpuIndex(const std::string& name) {
      if (!name.starts_with("cpu") || name.size() == 3) {
        return std::nullopt;
      }
      int index = 0;
      const auto [end, ec] = std::from_chars(name.data() + 3, name.data() + name.size(), index);
      if (ec != std::errc() || end != name.data() + name.size()) {
        return std::nullopt;
      }
      return index;
    }

  } // namespace

  std::vector<int> efficiencyCpus(const std::filesystem::path& cpuRoot) {
    struct Cpu {
      long long maxFreq;
      std::string cluster;
    };
    std::map<int, Cpu> cpus;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(cpuRoot, ec)) {
      const auto index = cpuIndex(entry.path().filename().string());
      if (!index) {
        continue;
      }
      const auto freq = readNumber(entry.path() / "cpufreq/cpuinfo_max_freq");
      if (!freq) {
        continue;
      }
      cpus[*index] = {*freq, readLine(entry.path() / "topology/cluster_cpus_list")};
    }
    if (cpus.empty()) {
      return {};
    }
    const auto [minIt, maxIt] = std::ranges::minmax_element(cpus, {}, [](const auto& cpu) { return cpu.second.maxFreq; });
    const long long lowest = minIt->second.maxFreq;
    const long long highest = maxIt->second.maxFreq;
    if (lowest == highest) {
      return {};
    }
    std::vector<std::string> performanceClusters;
    for (const auto& [index, cpu] : cpus) {
      if (cpu.maxFreq == highest) {
        performanceClusters.push_back(cpu.cluster);
      }
    }
    std::vector<int> result;
    for (const auto& [index, cpu] : cpus) {
      if (cpu.maxFreq == lowest && std::ranges::find(performanceClusters, cpu.cluster) == performanceClusters.end()) {
        result.push_back(index);
      }
    }
    if (result.size() < 2) {
      return {};
    }
    return result;
  }

} // namespace andromeda::qos
