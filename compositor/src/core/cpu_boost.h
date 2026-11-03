#pragma once

#include <string>
#include <string_view>
#include <sys/types.h>

struct sd_bus;
struct wl_event_loop;
struct wl_event_source;

namespace andromeda {

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

  private:
    void setWeight(const std::string& unit, unsigned weight);
    void dispatch();
    void updateEvents();
    static int onEvent(int fd, unsigned mask, void* data);

    sd_bus* m_bus = nullptr;
    wl_event_source* m_source = nullptr;
    std::string m_focused;
  };

} // namespace andromeda
