#include "system/activity_service.h"

#include "core/shared_worker.h"

#include <algorithm>
#include <thread>
#include <utility>

ActivityService::ActivityService(std::string cgroupRoot, int cpuCount)
    : m_cgroupRoot(std::move(cgroupRoot)),
      m_cpuCount(cpuCount > 0 ? cpuCount : std::max(1, static_cast<int>(std::thread::hardware_concurrency()))) {}

void ActivityService::setUnits(std::vector<SystemdUnit> units) {
  m_units = std::move(units);
  std::erase_if(m_history, [this](const auto& entry) {
    return std::ranges::none_of(m_units, [&entry](const SystemdUnit& u) { return u.name == entry.first; });
  });
  rebuild();
}

void ActivityService::setWindowUnits(std::map<std::string, std::string> windowAppIdByUnit) {
  m_windowAppIdByUnit = std::move(windowAppIdByUnit);
  rebuild();
}

void ActivityService::setUnitTiers(std::map<std::string, std::string> tierByUnit) {
  if (tierByUnit != m_tierByUnit) {
    m_tierByUnit = std::move(tierByUnit);
    rebuild();
  }
}

std::string_view qosTierName(std::string_view tier) {
  if (tier == "T0") {
    return "focused";
  }
  if (tier == "T1") {
    return "visible";
  }
  if (tier == "T2") {
    return "hidden";
  }
  if (tier == "T3") {
    return "hidden-long";
  }
  return {};
}

ActivityService::Readings ActivityService::read(const std::string& cgroupRoot, const std::vector<SystemdUnit>& units) {
  Readings out;
  out.reserve(units.size());
  for (const auto& unit : units) {
    out.emplace_back(unit.name, readCgroupSample(cgroupRoot, unit.controlGroup));
  }
  return out;
}

void ActivityService::sample(std::chrono::steady_clock::time_point now) { apply(now, read(m_cgroupRoot, m_units)); }

void ActivityService::sampleAsync(std::chrono::steady_clock::time_point now, std::function<void()> done) {
  if (m_reading) {
    return;
  }
  m_reading = true;
  SharedWorker::instance().post<Readings>(
      m_alive,
      [root = m_cgroupRoot, units = m_units]() { return read(root, units); },
      [this, now, done = std::move(done)](Readings readings) {
        m_reading = false;
        apply(now, std::move(readings));
        if (done) {
          done();
        }
      }
  );
}

void ActivityService::apply(std::chrono::steady_clock::time_point now, Readings readings) {
  for (auto& [name, reading] : readings) {
    auto& history = m_history[name];
    history.push_back({now, std::move(reading)});
    // Keep one sample at or before the window start so the rate spans the full window.
    while (history.size() > 2 && now - history[1].at >= kRateWindow) {
      history.pop_front();
    }
  }
  rebuild();
}

void ActivityService::rebuild() {
  std::map<std::string, ActivityRow> byKey;
  for (const auto& unit : m_units) {
    const auto window = m_windowAppIdByUnit.find(unit.name);
    const std::string appId = !unit.appId.empty() ? unit.appId : window != m_windowAppIdByUnit.end() ? window->second : "";
    const std::string key = appId.empty() ? unit.name : appId;
    ActivityRow& row = byKey[key];
    if (row.units.empty()) {
      row.key = key;
      row.appId = appId;
      row.slice = classifySlice(unit.slice);
      row.systemManager = unit.systemManager;
      row.startedUsec = unit.activeEnterUsec;
    } else if (unit.activeEnterUsec != 0 && (row.startedUsec == 0 || unit.activeEnterUsec < row.startedUsec)) {
      row.startedUsec = unit.activeEnterUsec;
    }
    row.units.push_back(unit.name);
    row.hasWindow = row.hasWindow || window != m_windowAppIdByUnit.end();
    if (const auto tier = m_tierByUnit.find(unit.name);
        tier != m_tierByUnit.end() && (row.qosTier.empty() || tier->second < row.qosTier)) {
      row.qosTier = tier->second;
    }

    const auto history = m_history.find(unit.name);
    if (history == m_history.end() || history->second.empty() || !history->second.back().sample.valid) {
      continue;
    }
    const Timed& last = history->second.back();
    row.memoryBytes += last.sample.memoryBytes;
    row.cpuUsageUsec += last.sample.cpuUsageUsec;
    row.memoryPressure = std::max(row.memoryPressure, last.sample.memoryPressureAvg10);
    const Timed& first = history->second.front();
    const double seconds = std::chrono::duration<double>(last.at - first.at).count();
    if (seconds <= 0.0 || !first.sample.valid) {
      continue;
    }
    const auto delta = [](std::uint64_t now, std::uint64_t before) { return now > before ? static_cast<double>(now - before) : 0.0; };
    row.cpuPercent += delta(last.sample.cpuUsageUsec, first.sample.cpuUsageUsec) / (seconds * 1e6 * m_cpuCount) * 100.0;
    row.ioReadBytesPerSec += delta(last.sample.ioReadBytes, first.sample.ioReadBytes) / seconds;
    row.ioWriteBytesPerSec += delta(last.sample.ioWriteBytes, first.sample.ioWriteBytes) / seconds;
  }
  m_rows.clear();
  for (auto& [key, row] : byKey) {
    m_rows.push_back(std::move(row));
  }
  std::ranges::sort(m_rows, [](const ActivityRow& a, const ActivityRow& b) {
    return a.memoryBytes != b.memoryBytes ? a.memoryBytes > b.memoryBytes : a.key < b.key;
  });
}
