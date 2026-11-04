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
    bool operator==(const TierSettings&) const = default;
  };

  // efficiencyCpus: from efficiencyCpus() in cpu_topology.h; empty on a non-hybrid CPU, so T2 and T3 keep every CPU.
  [[nodiscard]] TierSettings settingsFor(Tier tier, const std::vector<int>& efficiencyCpus);

} // namespace andromeda::qos
