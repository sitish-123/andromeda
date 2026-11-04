#include "qos/actuation.h"

#include <utility>

namespace andromeda::qos {

  TierSettings settingsFor(Tier tier, const std::vector<int>& efficiencyCpus) {
    switch (tier) {
    case Tier::Focus:
      return {.cpuWeight = 400, .ioWeight = 500, .allowedCpus = {}};
    case Tier::Visible:
      return {.cpuWeight = 100, .ioWeight = 100, .allowedCpus = {}};
    case Tier::Hidden:
      return {.cpuWeight = 30, .ioWeight = 10, .allowedCpus = efficiencyCpus};
    case Tier::HiddenLong:
      break;
    }
    std::vector<int> one;
    if (!efficiencyCpus.empty()) {
      one.push_back(efficiencyCpus.front());
    }
    return {.cpuWeight = 30, .ioWeight = 10, .allowedCpus = std::move(one)};
  }

} // namespace andromeda::qos
