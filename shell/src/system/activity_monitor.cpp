#include "system/activity_monitor.h"

#include "dbus/session_bus.h"
#include "system/cgroup_stats.h"

#include <chrono>
#include <map>
#include <nlohmann/json.hpp>

namespace {

  constexpr auto kSampleInterval = std::chrono::seconds(1);
  constexpr auto kLease = std::chrono::seconds(10);

  const char* sliceName(UnitSlice slice) {
    switch (slice) {
    case UnitSlice::App:
      return "app";
    case UnitSlice::Background:
      return "background";
    case UnitSlice::Session:
      return "session";
    case UnitSlice::Other:
      break;
    }
    return "other";
  }

} // namespace

ActivityMonitor::ActivityMonitor(SessionBus& bus, WindowPids windowPids, std::string cgroupRoot, ChangeCallback onChange)
    : m_windowPids(std::move(windowPids)), m_onChange(std::move(onChange)),
      m_userUnits(bus.connection(), SystemdUnitsService::Manager::User, [this]() {
        m_activity.setUnits(m_userUnits.units());
        if (!m_onceWaiters.empty()) {
          if (!m_sampling) {
            windowsChanged();
            m_activity.sample(std::chrono::steady_clock::now());
          }
          for (auto waiters = std::exchange(m_onceWaiters, {}); auto& done : waiters) {
            done();
          }
        }
        if (m_onChange) {
          m_onChange();
        }
      }),
      m_activity(std::move(cgroupRoot)) {}

ActivityMonitor::~ActivityMonitor() = default;

void ActivityMonitor::retain() {
  ++m_refs;
  updateSampling();
}

void ActivityMonitor::release() {
  if (m_refs > 0) {
    --m_refs;
  }
  updateSampling();
}

void ActivityMonitor::touch() {
  m_leased = true;
  m_leaseTimer.start(std::chrono::duration_cast<std::chrono::milliseconds>(kLease), [this]() {
    m_leased = false;
    updateSampling();
  });
  updateSampling();
}

void ActivityMonitor::sampleOnce(std::function<void()> done) {
  m_onceWaiters.push_back(std::move(done));
  if (m_onceWaiters.size() == 1) {
    m_userUnits.refresh();
  }
}

void ActivityMonitor::updateSampling() {
  const bool want = m_refs > 0 || m_leased;
  if (want == m_sampling) {
    return;
  }
  m_sampling = want;
  m_userUnits.setLive(want);
  if (!want) {
    m_sampleTimer.stop();
    return;
  }
  m_userUnits.refresh();
  sampleNow();
  m_sampleTimer.startRepeating(std::chrono::duration_cast<std::chrono::milliseconds>(kSampleInterval), [this]() { sampleNow(); });
}

void ActivityMonitor::sampleNow() {
  windowsChanged();
  m_activity.sample(std::chrono::steady_clock::now());
  if (m_onChange) {
    m_onChange();
  }
}

void ActivityMonitor::windowsChanged() {
  std::map<std::string, std::string> byUnit;
  for (const auto& [pid, appId] : m_windowPids()) {
    if (pid <= 0) {
      continue;
    }
    if (std::string unit = unitForPid(pid); !unit.empty()) {
      byUnit.try_emplace(std::move(unit), appId);
    }
  }
  m_activity.setWindowUnits(std::move(byUnit));
}

std::string ActivityMonitor::dumpJson() const {
  nlohmann::json rows = nlohmann::json::array();
  for (const auto& row : m_activity.rows()) {
    rows.push_back({
        {"key", row.key},
        {"app_id", row.appId},
        {"units", row.units},
        {"has_window", row.hasWindow},
        {"system", row.systemManager},
        {"slice", sliceName(row.slice)},
        {"started_usec", row.startedUsec},
        {"cpu_percent", row.cpuPercent},
        {"memory_bytes", row.memoryBytes},
        {"io_read_bps", row.ioReadBytesPerSec},
        {"io_write_bps", row.ioWriteBytesPerSec},
        {"memory_pressure", row.memoryPressure},
    });
  }
  return nlohmann::json{{"available", m_userUnits.available()}, {"sampling", m_sampling}, {"rows", rows}}.dump();
}
