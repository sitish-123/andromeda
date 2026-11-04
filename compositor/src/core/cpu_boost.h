#pragma once

#include <chrono>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <vector>

struct sd_bus;
struct wl_event_loop;
struct wl_event_source;

namespace andromeda {

  // systemd-run argument for a launching app unit: 10x the default weight (the shell's process.cpp sends the same).
  inline constexpr const char* kLaunchWeightProperty = "--property=CPUWeight=1000";

  // CPU weights of app units, like macOS QoS classes (.agent/TASK-boost.md): the shell starts an app unit at the
  // launch weight; this class moves the focused app unit to the focus weight, the one it leaves back to the default,
  // and drops a launch that never mapped back to the default. Calls go over the user bus asynchronously, so the
  // compositor never waits on systemd. Without sd-bus every call is a no-op.
  class CpuBoost {
  public:
    explicit CpuBoost(wl_event_loop* loop);
    ~CpuBoost();
    CpuBoost(const CpuBoost&) = delete;
    CpuBoost& operator=(const CpuBoost&) = delete;

    // Keyboard focus moved to a client. Focus on a non-app unit (the shell's panels) keeps the last app's boost.
    void focus(pid_t pid);
    // A launch of `appId` ended without a window (5 s): its app units go back to the default weight.
    void expireLaunch(std::string_view appId);
    // A compositor spawn started `unit` at the launch weight. Its first window (launchMapped with the spawn's
    // activation token) or the 5 s limit moves it to the default weight, unless it has the focus.
    void beginLaunch(std::string token, std::string unit);
    void launchMapped(std::string_view token);
    // False without a user bus: nothing could reset a launch weight, so spawns must not set one.
    [[nodiscard]] bool active() const { return m_bus != nullptr && m_loop != nullptr; }

  private:
    struct Launch {
      std::string token;
      std::string unit;
      std::chrono::steady_clock::time_point deadline;
    };
    void endLaunch(const Launch& launch);
    void armLaunchTimer();
    static int onLaunchTimer(void* data);
    void setWeight(const std::string& unit, unsigned weight);
    void dispatch();
    void updateEvents();
    static int onEvent(int fd, unsigned mask, void* data);

    sd_bus* m_bus = nullptr;
    wl_event_source* m_source = nullptr;
    wl_event_loop* m_loop = nullptr;
    wl_event_source* m_launchTimer = nullptr;
    std::vector<Launch> m_launches;
    std::string m_focused;
  };

} // namespace andromeda
