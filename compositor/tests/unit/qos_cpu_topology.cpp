#include "qos/cpu_topology.h"

#include "check.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

using andromeda::qos::efficiencyCpus;

namespace {

  struct FakeCpu {
    long long maxFreq;
    std::string cluster;
  };

  // A throwaway copy of /sys/devices/system/cpu with only the two files the detection reads.
  std::filesystem::path fakeSysfs(const std::vector<FakeCpu>& cpus) {
    std::string tmpl = (std::filesystem::temp_directory_path() / "qos-cpu-XXXXXX").string();
    const std::filesystem::path root = mkdtemp(tmpl.data());
    for (std::size_t i = 0; i < cpus.size(); ++i) {
      const auto dir = root / ("cpu" + std::to_string(i));
      std::filesystem::create_directories(dir / "cpufreq");
      std::filesystem::create_directories(dir / "topology");
      std::ofstream(dir / "cpufreq/cpuinfo_max_freq") << cpus[i].maxFreq << '\n';
      std::ofstream(dir / "topology/cluster_cpus_list") << cpus[i].cluster << '\n';
    }
    std::filesystem::create_directories(root / "cpufreq"); // not a CPU
    return root;
  }

} // namespace

ANDROMEDA_TEST(hybridCpuReportsItsLowPowerCluster) {
  // Meteor Lake-like: 6 P-cores, 8 E-cores in two clusters, 2 LP-E cores.
  std::vector<FakeCpu> cpus;
  for (int i = 0; i < 6; ++i) {
    cpus.push_back({5100000, std::to_string(i)});
  }
  for (int i = 6; i < 14; ++i) {
    cpus.push_back({4400000, i < 10 ? "6-9" : "10-13"});
  }
  cpus.push_back({2500000, "14-15"});
  cpus.push_back({2500000, "14-15"});
  const auto root = fakeSysfs(cpus);
  CHECK(efficiencyCpus(root) == std::vector<int>({14, 15}));
  std::filesystem::remove_all(root);
}

ANDROMEDA_TEST(uniformCpuHasNoEfficiencyCores) {
  const auto root = fakeSysfs({{3000000, "0-3"}, {3000000, "0-3"}, {3000000, "0-3"}, {3000000, "0-3"}});
  CHECK(efficiencyCpus(root).empty());
  std::filesystem::remove_all(root);
}

ANDROMEDA_TEST(aSingleSlowCpuOrOneInAPerformanceClusterDoesNotCount) {
  const auto single = fakeSysfs({{5000000, "0"}, {5000000, "1"}, {2000000, "2"}});
  CHECK(efficiencyCpus(single).empty());
  std::filesystem::remove_all(single);
  const auto shared = fakeSysfs({{5000000, "0-2"}, {2000000, "0-2"}, {2000000, "0-2"}});
  CHECK(efficiencyCpus(shared).empty());
  std::filesystem::remove_all(shared);
}

ANDROMEDA_TEST(missingSysfsGivesNothing) { CHECK(efficiencyCpus("/nonexistent-qos-root").empty()); }

int main() { return RUN_TESTS(); }
