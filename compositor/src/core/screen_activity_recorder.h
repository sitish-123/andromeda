#pragma once

#include "protocols/app_energy_log.h"
#include "protocols/screen_activity_log.h"

#include <cstdint>

struct wl_event_loop;
struct wl_event_source;

namespace andromeda {

  // Records screen on / idle time for Settings > Battery and writes screenActivityPath(). The compositor owns outputs
  // and input, so it is the only writer. One timer wakeup per minute at most, and only while the user is active.
  // Each write also books the app units' CPU time to appEnergyPath().
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

    ScreenActivityLog m_log;
    AppEnergyLog m_energy;
    wl_event_source* m_timer = nullptr;
    std::int64_t m_lastInput = 0;
    std::int64_t m_writtenAt = 0;
    bool m_idle = false;
    bool m_timerArmed = false;
  };

} // namespace andromeda
