#include "core/cpu_boost.h"

#include "core/log.h"
#include "core/systemd_units.h"

#include <algorithm>
#include <cctype>
#include <format>
#include <fstream>
#include <wayland-server-core.h>

namespace andromeda {

  namespace {
    constexpr Logger kLog("cpu-boost");

    // A sched_ext scheduler replaces the kernel's for normal tasks and may ignore cgroup cpu.weight (scx_lavd does).
    void logSchedExt() {
      std::ifstream state("/sys/kernel/sched_ext/state");
      std::string value;
      if (!std::getline(state, value) || value != "enabled") {
        return;
      }
      std::ifstream ops("/sys/kernel/sched_ext/root/ops");
      std::string name;
      std::getline(ops, name);
      kLog.warn("sched_ext scheduler {} runs: app CPU weights may have no effect", name.empty() ? "?" : name);
    }

    constexpr std::uint64_t kDefaultWeight = 100;
    // As Server::beginLaunchBusy: a launch without a window by then is not waiting on its first frame.
    constexpr std::chrono::seconds kLaunchLimit{5};

    // systemd unit-name escaping, as the shell does for app-<desktop id>@<uuid>.service.
    std::string escapeUnitName(std::string_view name) {
      std::string out;
      for (std::size_t i = 0; i < name.size(); ++i) {
        const auto c = static_cast<unsigned char>(name[i]);
        if ((std::isalnum(c) != 0 || c == ':' || c == '_' || (c == '.' && i > 0))) {
          out += static_cast<char>(c);
        } else {
          out += std::format("\\x{:02x}", c);
        }
      }
      return out;
    }
  } // namespace

  CpuBoost::CpuBoost(wl_event_loop* loop, SystemdUnits& units, LaunchEnded launchEnded)
      : m_units(units), m_launchEnded(std::move(launchEnded)), m_loop(loop) {
    logSchedExt();
  }

  CpuBoost::~CpuBoost() {
    if (m_launchTimer != nullptr) {
      wl_event_source_remove(m_launchTimer);
    }
  }

  bool CpuBoost::active() const { return m_units.active() && m_loop != nullptr; }

  bool CpuBoost::launching(const std::string& unit) const {
    return std::ranges::find(m_launches, unit, &Launch::unit) != m_launches.end();
  }

  void CpuBoost::endLaunch(const std::string& unit) {
    if (!m_launchEnded || !m_launchEnded(unit)) {
      m_units.setProperties(unit, {.cpuWeight = kDefaultWeight, .ioWeight = std::nullopt, .allowedCpus = std::nullopt, .memoryLow = std::nullopt, .memoryMin = std::nullopt});
    }
  }

  void CpuBoost::expireLaunch(std::string_view appId) {
    if (appId.empty() || !active()) {
      return;
    }
    m_units.listUnits({"app-" + escapeUnitName(appId) + "@*.service"}, [this](const std::string& unit) {
      endLaunch(unit);
    });
  }

  void CpuBoost::beginLaunch(std::string token, std::string unit) {
    if (!active() || token.empty() || unit.empty()) {
      return;
    }
    m_launches.push_back({std::move(token), std::move(unit), std::chrono::steady_clock::now() + kLaunchLimit});
    armLaunchTimer();
  }

  void CpuBoost::launchMapped(std::string_view token) {
    const auto match = std::ranges::find(m_launches, token, &Launch::token);
    if (match == m_launches.end()) {
      return;
    }
    const std::string unit = match->unit;
    m_launches.erase(match);
    armLaunchTimer();
    endLaunch(unit);
  }

  void CpuBoost::armLaunchTimer() {
    if (m_launchTimer == nullptr) {
      m_launchTimer = wl_event_loop_add_timer(m_loop, onLaunchTimer, this);
    }
    if (m_launchTimer == nullptr) {
      return;
    }
    int delayMs = 0;
    if (!m_launches.empty()) {
      const auto next = std::ranges::min(m_launches, {}, &Launch::deadline).deadline;
      const auto left = std::chrono::ceil<std::chrono::milliseconds>(next - std::chrono::steady_clock::now());
      delayMs = std::max(1, static_cast<int>(left.count()));
    }
    wl_event_source_timer_update(m_launchTimer, delayMs);
  }

  int CpuBoost::onLaunchTimer(void* data) {
    auto* self = static_cast<CpuBoost*>(data);
    const auto now = std::chrono::steady_clock::now();
    std::vector<std::string> ended;
    std::erase_if(self->m_launches, [&ended, now](const Launch& launch) {
      if (launch.deadline > now) {
        return false;
      }
      ended.push_back(launch.unit);
      return true;
    });
    self->armLaunchTimer();
    for (const std::string& unit : ended) {
      self->endLaunch(unit);
    }
    return 0;
  }

} // namespace andromeda
