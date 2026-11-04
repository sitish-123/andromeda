#include "qos/actuator.h"

#include "core/log.h"
#include "core/systemd_units.h"

#include <cerrno>
#include <filesystem>
#include <fstream>
#include <utility>

#include <fcntl.h>
#include <unistd.h>

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
      m_units.setProperties(unit, {.cpuWeight = 100, .ioWeight = 100, .allowedCpus = std::vector<int>{}, .memoryLow = 0});
      if (applied.timerSlackNs != 0) {
        writeTimerSlack(applied.cgroupDir, 0);
      }
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
    if (fresh || applied.memoryLow != settings.memoryLow) {
      properties.memoryLow = settings.memoryLow;
    }
    applied.cpuWeight = settings.cpuWeight;
    applied.ioWeight = settings.ioWeight;
    applied.allowedCpus = settings.allowedCpus;
    applied.memoryLow = settings.memoryLow;
    if (fresh || applied.timerSlackNs != settings.timerSlackNs) {
      applied.timerSlackNs = settings.timerSlackNs;
      writeTimerSlack(cgroupDir, settings.timerSlackNs);
    }
    if (applied.frozenWanted != frozen) {
      applied.freezerFailures = 0;
    }
    applied.frozenWanted = frozen;
    if (!frozen) {
      syncFreezer(unit);
    }
    if (properties.cpuWeight || properties.ioWeight || properties.allowedCpus || properties.memoryLow) {
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

  void Actuator::writeTimerSlack(const std::string& cgroupDir, std::uint64_t ns) {
    if (m_timerSlackDenied || cgroupDir.empty()) {
      return;
    }
    std::ifstream procs(cgroupDir + "/cgroup.procs");
    const std::string value = std::to_string(ns);
    std::error_code ec;
    for (std::string pid; std::getline(procs, pid);) {
      // Tasks exit and threads come and go during the walk: a missing one is skipped.
      for (std::filesystem::directory_iterator task("/proc/" + pid + "/task", ec), end; !ec && task != end;
           task.increment(ec)) {
        const int fd = open((task->path() / "timerslack_ns").c_str(), O_WRONLY | O_CLOEXEC);
        if (fd < 0) {
          continue;
        }
        const bool written = write(fd, value.data(), value.size()) == static_cast<ssize_t>(value.size());
        const int error = errno;
        close(fd);
        if (!written && error == EPERM) {
          m_timerSlackDenied = true;
          kLog.info("no timer slack for hidden apps: the compositor lacks CAP_SYS_NICE");
          return;
        }
      }
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
      m_units.setProperties(unit, {.cpuWeight = 100, .ioWeight = 100, .allowedCpus = std::vector<int>{}, .memoryLow = 0});
      m_units.setFrozen(unit, false);
    });
  }

} // namespace andromeda::qos
