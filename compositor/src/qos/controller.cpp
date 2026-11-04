#include "qos/controller.h"

#include "core/application_scope.h"
#include "core/log.h"
#include "core/systemd_units.h"
#include "qos/actuation.h"
#include "qos/cpu_topology.h"

#include <unistd.h>
#include <wayland-server-core.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <fstream>
#include <iterator>
#include <utility>

namespace andromeda::qos {

  namespace {

    constexpr Logger kLog("qos");
    // No event reports a battery change here: while a scope can move between T2 and T3, the state is read this often.
    constexpr std::uint64_t kBatteryRecheckMs = 60'000;

    std::uint64_t nowMs() {
      using namespace std::chrono;
      return static_cast<std::uint64_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
    }

    // Test-only: ANDROMEDA_QOS_TEST_DELAYS_MS="<hidden>,<hidden long>" shortens the policy delays.
    Delays delaysFromEnvironment() {
      Delays delays;
      if (const char* value = std::getenv("ANDROMEDA_QOS_TEST_DELAYS_MS")) {
        unsigned long long hidden = 0;
        unsigned long long hiddenLong = 0;
        if (std::sscanf(value, "%llu,%llu", &hidden, &hiddenLong) == 2) {
          delays = {.hiddenMs = hidden, .hiddenLongMs = hiddenLong};
        }
      }
      return delays;
    }

    const char* tierName(Tier tier) {
      switch (tier) {
      case Tier::Focus:
        return "T0";
      case Tier::Visible:
        return "T1";
      case Tier::Hidden:
        return "T2";
      case Tier::HiddenLong:
        return "T3";
      }
      return "?";
    }

    std::string cpuList(const std::vector<int>& cpus) {
      std::string out;
      for (const int cpu : cpus) {
        out += (out.empty() ? "" : ",") + std::to_string(cpu);
      }
      return out.empty() ? "none" : out;
    }


    std::string readFile(const std::string& path) {
      std::ifstream stream(path);
      return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
    }

    // cgroup v2 line: 0::/user.slice/.../<unit>[/<sub-cgroup>]
    std::string cgroupPath(const std::string& text) {
      const std::size_t line = text.find("0::");
      return line == std::string::npos ? std::string() : text.substr(line + 3, text.find('\n', line) - line - 3);
    }
  } // namespace

  Controller::Controller(wl_event_loop* loop, SystemdUnits& units, bool ownsSession, std::function<Snapshot()> snapshot)
      : m_loop(loop),
        m_units(units),
        m_actuator(units),
        m_snapshot(std::move(snapshot)),
        m_efficiencyCpus(efficiencyCpus("/sys/devices/system/cpu")),
        m_focusMemoryLow(static_cast<std::uint64_t>(sysconf(_SC_PHYS_PAGES)) * static_cast<std::uint64_t>(sysconf(_SC_PAGESIZE)) / 4),
        m_delays(delaysFromEnvironment()) {
    const char* powerSupply = std::getenv("ANDROMEDA_QOS_TEST_POWER_SUPPLY");
    m_powerSupply = powerSupply != nullptr ? powerSupply : "/sys/class/power_supply";
    m_timer = wl_event_loop_add_timer(loop, onTimer, this);
    const std::string own = readFile("/proc/self/cgroup");
    m_ownCgroup = cgroupPath(own);
    m_ownUnit = systemdUnitFromCgroup(own);
    kLog.info("efficiency CPUs for hidden apps: {}", cpuList(m_efficiencyCpus));
    // Session daemons run in background.slice (docs/qos.md §4). Only the session compositor sets it: a nested run
    // shares the user manager.
    m_pinsBackground = ownsSession && !m_efficiencyCpus.empty();
    if (m_pinsBackground) {
      units.setProperties("background.slice", {.cpuWeight = std::nullopt, .ioWeight = std::nullopt, .allowedCpus = m_efficiencyCpus, .memoryLow = std::nullopt});
    }
  }

  Controller::~Controller() {
    if (m_pinsBackground) {
      m_units.setProperties("background.slice", {.cpuWeight = std::nullopt, .ioWeight = std::nullopt, .allowedCpus = std::vector<int>{}, .memoryLow = std::nullopt});
    }
    if (m_idle != nullptr) {
      wl_event_source_remove(m_idle);
    }
    if (m_timer != nullptr) {
      wl_event_source_remove(m_timer);
    }
  }

  void Controller::schedule() {
    if (m_idle == nullptr) {
      m_idle = wl_event_loop_add_idle(m_loop, onIdle, this);
    }
  }

  void Controller::onIdle(void* data) {
    auto* self = static_cast<Controller*>(data);
    self->m_idle = nullptr;
    self->evaluate();
  }

  int Controller::onTimer(void* data) {
    static_cast<Controller*>(data)->evaluate();
    return 0;
  }

  void Controller::focus(pid_t pid) {
    if (pid <= 0) {
      return;
    }
    const Cgroup& cgroup = cgroupOf(pid, m_pids);
    if (!cgroup.unit.empty() && cgroup.unit != m_focused) {
      m_focused = cgroup.unit;
      schedule();
    }
  }

  void Controller::holdThawIntent(bool held) {
    m_thawHolds = std::max(0, m_thawHolds + (held ? 1 : -1));
    schedule();
  }

  bool Controller::launchEnded(const std::string& unit) {
    if (!m_scopes.contains(unit)) {
      return false;
    }
    m_actuator.invalidateCpuWeight(unit);
    schedule();
    return true;
  }

  void Controller::resetAppUnits(std::function<bool(const std::string&)> launching) {
    m_actuator.resetAppUnits([this, launching = std::move(launching)](const std::string& unit) {
      return m_scopes.contains(unit) || launching(unit);
    });
  }

  const Controller::Cgroup& Controller::cgroupOf(pid_t pid, std::unordered_map<pid_t, Cgroup>& seen) {
    if (auto it = seen.find(pid); it != seen.end()) {
      return it->second;
    }
    if (auto it = m_pids.find(pid); it != m_pids.end()) {
      return seen.emplace(pid, it->second).first->second;
    }
    Cgroup cgroup;
    const std::string text = readFile(std::format("/proc/{}/cgroup", pid));
    std::string unit = systemdUnitFromCgroup(text);
    const std::string path = cgroupPath(text);
    if (path == m_ownCgroup) {
      // A child the compositor spawned without a scope. Xwayland stays with the compositor.
      cgroup.adoptable = pid != getpid() && readFile(std::format("/proc/{}/comm", pid)) != "Xwayland\n";
    } else if (unit.starts_with("app-") && unit != m_ownUnit) {
      // The compositor's own unit (a nested run from a terminal) is never throttled or frozen.
      if (const std::size_t at = path.find("/" + unit); at != std::string::npos) {
        cgroup.dir = "/sys/fs/cgroup" + path.substr(0, at + 1 + unit.size());
        cgroup.unit = std::move(unit);
      }
    }
    return seen.emplace(pid, std::move(cgroup)).first->second;
  }

  void Controller::adopt(pid_t pid) {
    if (!m_adopting.insert(pid).second) {
      return;
    }
    const std::string unit = std::format("app-andromeda-{}.scope", pid);
    kLog.debug("pid {} has no app scope: moving it to {}", pid, unit);
    m_units.startScope(unit, pid, [this, pid](bool ok) {
      if (ok) {
        m_adopting.erase(pid);
        m_pids.erase(pid);
        schedule();
      }
    });
  }

  bool Controller::onBattery() const {
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(m_powerSupply, error)) {
      const auto read = [&entry](const char* name) {
        std::ifstream stream(entry.path() / name);
        std::string value;
        std::getline(stream, value);
        return value;
      };
      const std::string scope = read("scope");
      // A peripheral (scope Device) reports its own battery.
      if (read("type") == "Battery" && (scope.empty() || scope == "System") && read("status") == "Discharging") {
        return true;
      }
    }
    return false;
  }

  void Controller::arm(std::optional<std::uint64_t> atMs, std::uint64_t nowMs) {
    if (m_timer == nullptr) {
      return;
    }
    // Zero disarms the timer.
    const int delayMs = atMs ? static_cast<int>(std::max<std::uint64_t>(1, *atMs > nowMs ? *atMs - nowMs : 1)) : 0;
    wl_event_source_timer_update(m_timer, delayMs);
  }

  void Controller::evaluate() {
    const Snapshot snapshot = m_snapshot();
    struct Gathered {
      AppInputs inputs;
      int windows = 0;
      bool neverThrottle = false;
      bool freezeWhenHidden = false;
    };
    std::unordered_map<std::string, Gathered> gathered;
    std::unordered_map<pid_t, Cgroup> seen;
    for (const Snapshot::Window& window : snapshot.windows) {
      const Cgroup& cgroup = cgroupOf(window.pid, seen);
      if (cgroup.unit.empty()) {
        if (cgroup.adoptable && window.adoptable) {
          adopt(window.pid);
        }
        continue;
      }
      m_scopes.try_emplace(cgroup.unit, m_delays).first->second.dir = cgroup.dir;
      Gathered& app = gathered[cgroup.unit];
      ++app.windows;
      app.inputs.visible = app.inputs.visible || !window.hidden;
      app.inputs.screenCast = app.inputs.screenCast || window.screenCast;
      app.neverThrottle = app.neverThrottle || window.rule == Rule::NeverThrottle;
      app.freezeWhenHidden = app.freezeWhenHidden || window.rule == Rule::FreezeWhenHidden;
    }
    // Audio and inhibitors often come from a child process (a browser's content process): matched by unit, not pid.
    for (const pid_t pid : snapshot.idleInhibitorPids) {
      if (const Cgroup& cgroup = cgroupOf(pid, seen); m_scopes.contains(cgroup.unit)) {
        gathered[cgroup.unit].inputs.idleInhibitor = true;
      }
    }
    for (const Snapshot::Activity& activity : snapshot.activity) {
      if (const Cgroup& cgroup = cgroupOf(activity.pid, seen); m_scopes.contains(cgroup.unit)) {
        AppInputs& inputs = gathered[cgroup.unit].inputs;
        inputs.audio = inputs.audio || activity.audio;
        inputs.call = inputs.call || activity.call;
      }
    }
    // Only the pids this snapshot names stay cached, so a reused pid is read again.
    m_pids = std::move(seen);

    const std::uint64_t now = nowMs();
    std::optional<bool> battery;
    std::optional<std::uint64_t> next;
    bool batteryCanMatter = false;
    for (auto it = m_scopes.begin(); it != m_scopes.end();) {
      const std::string& unit = it->first;
      Scope& scope = it->second;
      Gathered& app = gathered[unit];
      if (app.windows == 0) {
        std::error_code error;
        if (!std::filesystem::exists(scope.dir, error)) {
          kLog.debug("{} ended", unit);
          m_actuator.forget(unit);
          if (m_focused == unit) {
            m_focused.clear();
          }
          it = m_scopes.erase(it);
          continue;
        }
      } else {
        scope.rule = app.neverThrottle ? Rule::NeverThrottle
            : app.freezeWhenHidden     ? Rule::FreezeWhenHidden
                                       : Rule::Normal;
      }
      AppInputs inputs = app.inputs;
      inputs.focused = unit == m_focused && inputs.visible;
      inputs.thawIntent = m_thawHolds > 0;
      inputs.rule = scope.rule;
      // A background service: an app unit with no mapped window.
      if (app.windows == 0 && inputs.rule == Rule::Normal && snapshot.freezeBackgroundApps) {
        inputs.rule = Rule::FreezeWhenHidden;
      }
      const bool exempt = inputs.audio
          || inputs.call
          || inputs.idleInhibitor
          || inputs.screenCast
          || inputs.rule == Rule::NeverThrottle;
      bool discharging = false;
      if (!inputs.focused && !inputs.visible && !exempt) {
        if (!battery) {
          battery = onBattery();
        }
        discharging = *battery;
      }
      const Decision decision = scope.policy.update(inputs, discharging, now);
      if (decision.tier != scope.tier || decision.frozen != scope.frozen) {
        kLog.debug("{} {}{}", unit, tierName(decision.tier), decision.frozen ? " frozen" : "");
      }
      m_actuator.apply(unit, scope.dir, settingsFor(decision.tier, m_efficiencyCpus, m_focusMemoryLow), decision.frozen);
      scope.tier = decision.tier;
      scope.frozen = decision.frozen;
      if (decision.recheckAtMs) {
        next = std::min(next.value_or(*decision.recheckAtMs), *decision.recheckAtMs);
      }
      // Unplugging can move a T2 scope to T3; plugging in moves a T3 scope back.
      batteryCanMatter = batteryCanMatter
          || (decision.tier == Tier::Hidden && !discharging)
          || decision.tier == Tier::HiddenLong;
      ++it;
    }
    if (batteryCanMatter) {
      next = std::min(next.value_or(now + kBatteryRecheckMs), now + kBatteryRecheckMs);
    }
    arm(next, now);
  }

} // namespace andromeda::qos
