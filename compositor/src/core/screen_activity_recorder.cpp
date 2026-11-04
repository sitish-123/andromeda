#include "core/screen_activity_recorder.h"

#include "qos/holds.h"

#include <wayland-server-core.h>

#include <ctime>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
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

    std::string readFile(const std::string& path) {
      std::ifstream in(path);
      std::stringstream text;
      text << in.rdbuf();
      return text.str();
    }

    // Replaces `path` whole, so the shell never reads half a file.
    void writeFile(const std::filesystem::path& path, const std::string& text) {
      std::error_code ec;
      std::filesystem::create_directories(path.parent_path(), ec);
      const std::filesystem::path tmp = path.string() + ".tmp";
      {
        std::ofstream out(tmp, std::ios::trunc);
        out << text;
        if (!out) {
          return;
        }
      }
      std::filesystem::rename(tmp, path, ec);
    }

    std::optional<std::uint64_t> readUsageUsec(const std::string& cgroupDir) {
      std::ifstream in(cgroupDir + "/cpu.stat");
      std::string key;
      std::uint64_t value = 0;
      while (in >> key >> value) {
        if (key == "usage_usec") {
          return value;
        }
      }
      return std::nullopt;
    }

  } // namespace

  ScreenActivityRecorder::ScreenActivityRecorder(wl_event_loop* loop) {
    m_log.parse(readFile(screenActivityPath()));
    m_energy.parse(readFile(appEnergyPath()));
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
    writeFile(screenActivityPath(), m_log.snapshot(now));
    std::set<std::string> live;
    for (const auto& [unit, app] : qos::registry().apps) {
      if (const auto usage = readUsageUsec(app.dir); usage.has_value()) {
        m_energy.book(unit, *usage, now);
        live.insert(unit);
      }
    }
    m_energy.keepUnits(live);
    writeFile(appEnergyPath(), m_energy.serialize(now));
  }

} // namespace andromeda
