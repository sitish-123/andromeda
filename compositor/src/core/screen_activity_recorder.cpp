#include "core/screen_activity_recorder.h"

#include <wayland-server-core.h>

#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace andromeda {

  namespace {

    std::int64_t unixNow() {
      timespec ts{};
      clock_gettime(CLOCK_REALTIME_COARSE, &ts);
      return ts.tv_sec;
    }

    constexpr int kIdleCheckMs = static_cast<int>(ScreenActivityLog::kIdleAfter * 1000);
    // Input pauses flip idle often while the user types; the file is rewritten at most this often for them.
    constexpr std::int64_t kWriteEverySeconds = 300;

  } // namespace

  ScreenActivityRecorder::ScreenActivityRecorder(wl_event_loop* loop) {
    std::ifstream in(screenActivityPath());
    if (in) {
      std::stringstream text;
      text << in.rdbuf();
      m_log.parse(text.str());
    }
    const std::int64_t now = unixNow();
    m_lastInput = now;
    m_log.setScreenOn(true, now);
    m_timer = wl_event_loop_add_timer(loop, onIdleCheck, this);
    if (m_timer != nullptr) {
      wl_event_source_timer_update(m_timer, kIdleCheckMs);
      m_timerArmed = true;
    }
  }

  ScreenActivityRecorder::~ScreenActivityRecorder() {
    write(true);
    if (m_timer != nullptr) {
      wl_event_source_remove(m_timer);
    }
  }

  void ScreenActivityRecorder::setScreenOn(bool on) {
    m_log.setScreenOn(on, unixNow());
    write(true);
  }

  void ScreenActivityRecorder::inputActivity() {
    const std::int64_t now = unixNow();
    m_lastInput = now;
    if (m_idle) {
      m_idle = false;
      m_log.setUserIdle(false, now);
      write(false);
    }
    if (!m_timerArmed && m_timer != nullptr) {
      wl_event_source_timer_update(m_timer, kIdleCheckMs);
      m_timerArmed = true;
    }
  }

  int ScreenActivityRecorder::onIdleCheck(void* data) {
    auto* self = static_cast<ScreenActivityRecorder*>(data);
    const std::int64_t now = unixNow();
    const std::int64_t quiet = now - self->m_lastInput;
    if (quiet >= ScreenActivityLog::kIdleAfter) {
      // The pause began at the last input, so idle time starts there.
      self->m_idle = true;
      self->m_log.setUserIdle(true, self->m_lastInput);
      self->m_timerArmed = false;
      self->write(false);
    } else {
      wl_event_source_timer_update(self->m_timer, static_cast<int>((ScreenActivityLog::kIdleAfter - quiet) * 1000));
    }
    return 0;
  }

  void ScreenActivityRecorder::write(bool force) {
    const std::int64_t now = unixNow();
    if (!force && now - m_writtenAt < kWriteEverySeconds) {
      return;
    }
    m_writtenAt = now;
    const std::filesystem::path path = screenActivityPath();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    const std::filesystem::path tmp = path.string() + ".tmp";
    {
      std::ofstream out(tmp, std::ios::trunc);
      out << m_log.snapshot(now);
      if (!out) {
        return;
      }
    }
    std::filesystem::rename(tmp, path, ec);
  }

} // namespace andromeda
