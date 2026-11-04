#pragma once

#include <filesystem>
#include <vector>

namespace andromeda::qos {

  // The efficiency CPUs that hidden apps move to (docs/qos.md §4): the CPUs with the lowest cpuinfo_max_freq whose
  // cluster holds no CPU of the highest frequency. Empty on a non-hybrid CPU or when fewer than 2 such CPUs exist.
  // `cpuRoot` is /sys/devices/system/cpu or a test copy of it.
  [[nodiscard]] std::vector<int> efficiencyCpus(const std::filesystem::path& cpuRoot);

} // namespace andromeda::qos
