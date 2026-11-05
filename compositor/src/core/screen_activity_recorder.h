#pragma once

#include "core/main_loop_worker.h"
#include "core/package_energy.h"
#include "protocols/app_energy_log.h"
#include "protocols/screen_activity_log.h"

#include <cstdint>
#include <functional>

struct wl_event_loop;
struct wl_event_source;

namespace andromeda {

  // Records screen on / idle time for Settings > Battery and writes screenActivityPath(). The compositor owns outputs
  // and input, so it is the only writer. One timer wakeup per minute at most, and only while the user is active.
  // Each write also books the app units' and the machine's CPU time, and the CPU package energy, to appEnergyPath().
  class ScreenActivityRecorder {
  public:
    // `persist` = the session compositor: only it reads and writes the user's screen-time and app-energy files.
    ScreenActivityRecorder(wl_event_loop* loop, bool persist);
    ~ScreenActivityRecorder();

    ScreenActivityRecorder(const ScreenActivityRecorder&) = delete;
    ScreenActivityRecorder& operator=(const ScreenActivityRecorder&) = delete;

    void setScreenOn(bool on);
    // Every input event; costs one clock read unless the user was idle.
    void inputActivity();
    // The user left the input alone past ScreenActivityLog::kIdleAfter, or every screen is off.
    bool idle() const { return m_idle || !m_screenOn; }
    // Runs when idle() changes.
    void setIdleListener(std::function<void(bool)> listener) { m_idleListener = std::move(listener); }

  private:
    static int onIdleCheck(void* data);
    void write(bool force);
    void writeLog(std::int64_t now);
    // Scans every user process's fds (~19 ms for 100 processes) on m_worker, then books and writes on the loop.
    void bookGpu(std::int64_t now);
    // Books a suspend that ended before `now`: CLOCK_BOOTTIME runs on in suspend, CLOCK_MONOTONIC does not.
    void checkSleep(std::int64_t now);
    void reportIdle();

    ScreenActivityLog m_log;
    AppEnergyLog m_energy;
    PackageEnergy m_packageEnergy; // after m_energy: a reply in flight dies with it
    wl_event_source* m_timer = nullptr;
    std::int64_t m_lastInput = 0;
    std::int64_t m_writtenAt = 0;
    bool m_persist = false;
    std::int64_t m_sleptSeconds = 0; // BOOTTIME - MONOTONIC at the last check
    bool m_idle = false;
    bool m_screenOn = true;
    bool m_reportedIdle = false;
    std::function<void(bool)> m_idleListener;
    bool m_timerArmed = false;
    bool m_gpuScanBusy = false;
    bool m_closing = false; // the destructor books the GPU synchronously: no loop runs after it
    MainLoopWorker m_worker; // last: joined before the logs its results book into go away
  };

} // namespace andromeda
