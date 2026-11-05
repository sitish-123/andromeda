#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

struct wl_event_loop;
struct wl_event_source;

namespace andromeda {

  class SystemdUnits;

  // systemd-run argument for a launching app unit: 10x the default weight (the shell's process.cpp sends the same).
  inline constexpr const char* kLaunchWeightProperty = "--property=CPUWeight=1000";
  inline constexpr std::uint64_t kLaunchWeight = 1000; // the same weight for a scope started over D-Bus

  // The launch CPU boost (.agent/TASK-boost.md): an app unit runs at the launch weight until its first window or 5 s.
  // After that the QoS controller owns the unit's CPUWeight (docs/qos.md §3).
  class CpuBoost {
  public:
    // A launch ended for `unit`. Returns true when the QoS controller took the unit; otherwise it goes back to the
    // default weight here.
    using LaunchEnded = std::function<bool(const std::string& unit)>;

    CpuBoost(wl_event_loop* loop, SystemdUnits& units, LaunchEnded launchEnded);
    ~CpuBoost();
    CpuBoost(const CpuBoost&) = delete;
    CpuBoost& operator=(const CpuBoost&) = delete;

    // A launch of `appId` ended without a window (5 s): its app units leave the launch weight.
    void expireLaunch(std::string_view appId);
    // A compositor spawn started `unit` at the launch weight. Its first window (launchMapped with the spawn's
    // activation token) or the 5 s limit ends the launch.
    void beginLaunch(std::string token, std::string unit);
    void launchMapped(std::string_view token);
    // False without a user bus: nothing could end a launch weight, so spawns must not set one.
    [[nodiscard]] bool active() const;
    [[nodiscard]] bool launching(const std::string& unit) const;

  private:
    struct Launch {
      std::string token;
      std::string unit;
      std::chrono::steady_clock::time_point deadline;
    };
    void endLaunch(const std::string& unit);
    void armLaunchTimer();
    static int onLaunchTimer(void* data);

    SystemdUnits& m_units;
    LaunchEnded m_launchEnded;
    wl_event_loop* m_loop = nullptr;
    wl_event_source* m_launchTimer = nullptr;
    std::vector<Launch> m_launches;
  };

} // namespace andromeda
