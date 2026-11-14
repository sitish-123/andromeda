#pragma once

#include "qos/actuator.h"
#include "qos/on_battery_watch.h"
#include "qos/policy.h"

#include <sys/types.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct wl_event_loop;
struct wl_event_source;

namespace andromeda {
  class SystemdUnits;
}

namespace andromeda::qos {

  // What the compositor knows at one moment (docs/qos.md §2).
  struct Snapshot {
    struct Window {
      pid_t pid = 0;
      bool hidden = false; // as View::syncSuspendedState decides it
      bool screenCast = false;
      Rule rule = Rule::Normal;
      // May get its own scope: a native Wayland client that is not the shell. An X11 pid is the client's own claim.
      bool adoptable = false;
      bool thawShown = false; // the running workspace slide or the open app switcher can show it
      std::string appId;      // xdg app_id or X11 class; empty when the client sets none
    };
    struct Activity {
      pid_t pid = 0;
      bool audio = false;
      bool call = false;
    };
    std::vector<Window> windows; // mapped toplevels
    std::vector<pid_t> idleInhibitorPids;
    std::vector<Activity> activity; // dsk_shell_v1 app_activity
    bool freezeBackgroundApps = false;
    bool freezeHiddenApps = true;
    bool optimizeBackgroundApps = true;
  };

  // Owns the tier of every app unit that has mapped a window (docs/qos.md §3): its CPU and I/O weights, its CPUs and
  // its frozen state. It evaluates only when an input changes (schedule) or a policy delay ends; it never polls.
  class Controller {
  public:
    // ownsSession: the session's compositor, not a nested or test run; only it changes background.slice.
    Controller(wl_event_loop* loop, SystemdUnits& units, bool ownsSession, std::function<Snapshot()> snapshot);
    ~Controller();
    Controller(const Controller&) = delete;
    Controller& operator=(const Controller&) = delete;

    // An input changed: evaluate once the current dispatch ends.
    void schedule();
    // Keyboard focus moved to the client process pid. Focus on a process outside an app unit (the shell) keeps the
    // last app focused.
    void focus(pid_t pid);
    // The user starts to reach hidden apps (the app switcher opens, a workspace swipe starts): frozen apps with a
    // Snapshot::Window::thawShown window thaw until every hold ends.
    void holdThawIntent(bool held);
    // CpuBoost ended the launch weight of unit. Returns true when the unit is ours, so its tier weight goes back on.
    bool launchEnded(const std::string& unit);
    // At session start: app units of a previous compositor go back to the defaults and thaw.
    void resetAppUnits(std::function<bool(const std::string&)> launching);

  private:
    struct Cgroup {
      std::string unit; // empty: not an app unit
      std::string dir;
      bool adoptable = false; // not an app unit, but may get one (docs/qos.md §2)
    };
    struct Scope {
      explicit Scope(Delays delays) : policy(delays) {}
      ScopePolicy policy;
      std::string dir;
      Rule rule = Rule::Normal; // from its windows; kept while it has none
      std::string appId;        // from its windows; kept while it has none
      Tier tier = Tier::Visible;
      bool frozen = false;
    };

    static int onTimer(void* data);
    static void onIdle(void* data);
    void evaluate();
    const Cgroup& cgroupOf(pid_t pid, std::unordered_map<pid_t, Cgroup>& seen);
    void adopt(pid_t pid);
    [[nodiscard]] bool onBattery() const;
    void arm(std::optional<std::uint64_t> atMs, std::uint64_t nowMs);

    wl_event_loop* m_loop;
    SystemdUnits& m_units;
    Actuator m_actuator;
    std::function<Snapshot()> m_snapshot;
    wl_event_source* m_timer = nullptr;
    wl_event_source* m_idle = nullptr;
    std::unordered_map<pid_t, Cgroup> m_pids;
    // The /proc children walk of each window pid, kept while that pid has a window, as m_pids keeps its cgroup.
    std::unordered_map<pid_t, std::vector<pid_t>> m_children;
    std::unordered_set<pid_t> m_adopting; // a scope request is out, or it failed: not asked again while its window lives
    std::string m_ownCgroup;
    std::string m_ownUnit;
    bool m_pinsBackground = false;
    std::unordered_map<std::string, Scope> m_scopes;
    std::string m_focused;
    int m_thawHolds = 0;
    std::vector<int> m_efficiencyCpus;
    std::uint64_t m_focusMemoryLow; // 25 % of RAM (docs/qos.md §3)
    Delays m_delays;
    // Set only by ANDROMEDA_QOS_TEST_POWER_SUPPLY (a fixture directory); otherwise m_battery answers.
    std::filesystem::path m_powerSupply;
    std::unique_ptr<OnBatteryWatch> m_battery;
  };

} // namespace andromeda::qos
