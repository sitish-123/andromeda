#include "qos/actuation.h"

#include <utility>

namespace andromeda::qos {

  TierSettings settingsFor(Tier tier, const std::vector<int>& efficiencyCpus, std::uint64_t focusMemoryLow) {
    switch (tier) {
    case Tier::Focus:
      return {.cpuWeight = 400, .ioWeight = 500, .allowedCpus = {}, .timerSlackNs = 0, .memoryLow = focusMemoryLow, .memoryMin = focusMemoryLow / 2};
    case Tier::Visible:
      return {.cpuWeight = 100, .ioWeight = 100, .allowedCpus = {}, .timerSlackNs = 0, .memoryLow = 0};
    case Tier::Hidden:
      return {.cpuWeight = 30, .ioWeight = 10, .allowedCpus = efficiencyCpus, .timerSlackNs = 20'000'000, .memoryLow = 0};
    case Tier::HiddenLong:
      break;
    }
    std::vector<int> one;
    if (!efficiencyCpus.empty()) {
      one.push_back(efficiencyCpus.front());
    }
    return {.cpuWeight = 30, .ioWeight = 10, .allowedCpus = std::move(one), .timerSlackNs = 100'000'000, .memoryLow = 0};
  }

} // namespace andromeda::qos
