#include "qos/actuator.h"

#include "core/log.h"
#include "core/systemd_units.h"

#include <fstream>
#include <utility>

namespace andromeda::qos {

  namespace {
    constexpr Logger kLog("qos");
    constexpr int kFreezerRetries = 3;

    void writeThaw(const std::string& cgroupDir) {
      if (cgroupDir.empty()) {
        return;
      }
      std::ofstream freeze(cgroupDir + "/cgroup.freeze");
      freeze << "0";
      if (!freeze.flush()) {
        kLog.debug("thaw of {} failed", cgroupDir);
      }
    }
  } // namespace

  Actuator::~Actuator() {
    for (const auto& [unit, applied] : m_applied) {
      if (applied.frozenWanted || applied.frozenDone != false || applied.freezerBusy) {
        writeThaw(applied.cgroupDir);
        m_units.setFrozen(unit, false);
      }
      m_units.setProperties(unit, {.cpuWeight = 100, .ioWeight = 100, .allowedCpus = std::vector<int>{}});
    }
  }

  void Actuator::apply(const std::string& unit, const std::string& cgroupDir, const TierSettings& settings, bool frozen) {
    auto [it, fresh] = m_applied.try_emplace(unit);
    Applied& applied = it->second;
    if (fresh) {
      applied.id = ++m_nextId;
    }
    applied.cgroupDir = cgroupDir;
    SystemdUnits::Properties properties;
    if (fresh || applied.cpuWeight != settings.cpuWeight) {
      properties.cpuWeight = settings.cpuWeight;
    }
    if (fresh || applied.ioWeight != settings.ioWeight) {
      properties.ioWeight = settings.ioWeight;
    }
    if (fresh || applied.allowedCpus != settings.allowedCpus) {
      properties.allowedCpus = settings.allowedCpus;
    }
    applied.cpuWeight = settings.cpuWeight;
    applied.ioWeight = settings.ioWeight;
    applied.allowedCpus = settings.allowedCpus;
    if (applied.frozenWanted != frozen) {
      applied.freezerFailures = 0;
    }
    applied.frozenWanted = frozen;
    if (!frozen) {
      syncFreezer(unit);
    }
    if (properties.cpuWeight || properties.ioWeight || properties.allowedCpus) {
      m_units.setProperties(unit, properties);
    }
    if (frozen) {
      syncFreezer(unit);
    }
  }

  void Actuator::syncFreezer(const std::string& unit) {
    const auto it = m_applied.find(unit);
    if (it == m_applied.end()) {
      return;
    }
    Applied& applied = it->second;
    const bool frozen = applied.frozenWanted;
    if (!frozen && (applied.frozenDone != false || applied.freezerBusy)) {
      // ThawUnit takes a bus round trip and a systemd loop turn; the direct write thaws within the frame. It also
      // runs while a freeze is in flight, so a window shown meanwhile never waits for systemd.
      writeThaw(applied.cgroupDir);
    }
    if (applied.freezerBusy || applied.frozenDone == frozen) {
      return;
    }
    applied.freezerBusy = true;
    const bool sent = m_units.setFrozen(unit, frozen, [this, unit, id = applied.id, frozen](bool ok) {
      const auto found = m_applied.find(unit);
      if (found == m_applied.end() || found->second.id != id) {
        return;
      }
      Applied& state = found->second;
      state.freezerBusy = false;
      state.frozenDone = ok ? std::optional<bool>(frozen) : std::nullopt;
      state.freezerFailures = ok ? 0 : state.freezerFailures + 1;
      if (state.freezerFailures <= kFreezerRetries) {
        syncFreezer(unit);
      }
    });
    if (!sent) {
      applied.freezerBusy = false;
      applied.frozenDone.reset();
    }
  }

  void Actuator::invalidateCpuWeight(const std::string& unit) {
    if (auto it = m_applied.find(unit); it != m_applied.end()) {
      it->second.cpuWeight.reset();
    }
  }

  void Actuator::resetAppUnits(std::function<bool(const std::string&)> skip) {
    m_units.listUnits({"app-*.scope", "app-*.service"}, [this, skip = std::move(skip)](const std::string& unit) {
      if (skip(unit)) {
        return;
      }
      m_units.setProperties(unit, {.cpuWeight = 100, .ioWeight = 100, .allowedCpus = std::vector<int>{}});
      m_units.setFrozen(unit, false);
    });
  }

} // namespace andromeda::qos
