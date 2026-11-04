#pragma once

#include "core/package_energy.h"
#include "protocols/app_energy_log.h"
#include "protocols/screen_activity_log.h"

#include <cstdint>

struct wl_event_loop;
struct wl_event_source;

namespace andromeda {

  // Records screen on / idle time for Settings > Battery and writes screenActivityPath(). The compositor owns outputs
  // and input, so it is the only writer. One timer wakeup per minute at most, and only while the user is active.
  // Each write also books the app units' and the machine's CPU time, and the CPU package energy, to appEnergyPath().
  class ScreenActivityRecorder {
  public:
    explicit ScreenActivityRecorder(wl_event_loop* loop);
    ~ScreenActivityRecorder();

    ScreenActivityRecorder(const ScreenActivityRecorder&) = delete;
    ScreenActivityRecorder& operator=(const ScreenActivityRecorder&) = delete;

    void setScreenOn(bool on);
    // Every input event; costs one clock read unless the user was idle.
    void inputActivity();

  private:
    static int onIdleCheck(void* data);
    void write(bool force);
    // Books a suspend that ended before `now`: CLOCK_BOOTTIME runs on in suspend, CLOCK_MONOTONIC does not.
    void checkSleep(std::int64_t now);

    ScreenActivityLog m_log;
    AppEnergyLog m_energy;
    PackageEnergy m_packageEnergy; // after m_energy: a reply in flight dies with it
    wl_event_source* m_timer = nullptr;
    std::int64_t m_lastInput = 0;
    std::int64_t m_writtenAt = 0;
    std::int64_t m_sleptSeconds = 0; // BOOTTIME - MONOTONIC at the last check
    bool m_idle = false;
    bool m_timerArmed = false;
  };

} // namespace andromeda
