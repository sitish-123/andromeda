#pragma once

#include "qos/policy.h"

#include <cstdint>
#include <vector>

// The cgroup settings of each tier (docs/qos.md §3). Pure: no D-Bus.
namespace andromeda::qos {

  struct TierSettings {
    std::uint64_t cpuWeight = 100;
    std::uint64_t ioWeight = 100;
    std::vector<int> allowedCpus; // empty = every CPU
    std::uint64_t timerSlackNs = 0; // 0 = the thread's default (50 µs)
    std::uint64_t memoryLow = 0;    // bytes
    std::uint64_t memoryMin = 0;    // bytes
    bool operator==(const TierSettings&) const = default;
  };

  // efficiencyCpus: from efficiencyCpus() in cpu_topology.h; empty on a non-hybrid CPU, so T2 and T3 keep every CPU.
  // focusMemoryLow: the T0 MemoryLow in bytes (25 % of RAM); T0 MemoryMin is half of it.
  [[nodiscard]] TierSettings
  settingsFor(Tier tier, const std::vector<int>& efficiencyCpus, std::uint64_t focusMemoryLow);

} // namespace andromeda::qos
