#pragma once

#include "qos/actuation.h"

#include <functional>
#include <optional>
#include <string>
#include <unordered_map>

namespace andromeda {
  class SystemdUnits;
}

namespace andromeda::qos {

  // Sends tier settings and the frozen state of app units to the user manager (docs/qos.md §4), only what changed.
  class Actuator {
  public:
    explicit Actuator(SystemdUnits& units) : m_units(units) {}
    // Thaws and resets every unit it changed: a frozen app must not outlive the compositor frozen.
    ~Actuator();
    Actuator(const Actuator&) = delete;
    Actuator& operator=(const Actuator&) = delete;

    // cgroupDir: the unit's /sys/fs/cgroup directory; a thaw writes its cgroup.freeze at once, before systemd acts.
    void apply(const std::string& unit, const std::string& cgroupDir, const TierSettings& settings, bool frozen);
    // The next apply sends the CPU weight again: another writer (the launch boost) changed it.
    void invalidateCpuWeight(const std::string& unit);
    void forget(const std::string& unit) { m_applied.erase(unit); }
    // A restarted compositor has no record of what its predecessor set: every app unit for which `skip` is false
    // goes back to the defaults and is thawed.
    void resetAppUnits(std::function<bool(const std::string&)> skip);

  private:
    // One freezer call per unit at a time: systemd rejects a thaw while a freeze is in progress.
    struct Applied {
      std::uint64_t id = 0;
      std::string cgroupDir;
      std::optional<std::uint64_t> cpuWeight;
      std::uint64_t ioWeight = 0;
      std::vector<int> allowedCpus;
      bool frozenWanted = false;
      std::optional<bool> frozenDone = false; // the state systemd last confirmed; empty after a failed call
      bool freezerBusy = false;
      int freezerFailures = 0; // in a row; retried at once up to kFreezerRetries
    };
    void syncFreezer(const std::string& unit);

    SystemdUnits& m_units;
    std::unordered_map<std::string, Applied> m_applied;
    std::uint64_t m_nextId = 0;
  };

} // namespace andromeda::qos
