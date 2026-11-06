#include "core/screen_activity_recorder.h"

#include "core/drm_clients.h"
#include "qos/holds.h"

#include <unistd.h>
#include <wayland-server-core.h>

#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
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

    // Seconds the machine spent suspended since boot.
    std::int64_t sleptSeconds() {
      timespec boot{};
      timespec mono{};
      clock_gettime(CLOCK_BOOTTIME, &boot);
      clock_gettime(CLOCK_MONOTONIC, &mono);
      return boot.tv_sec - mono.tv_sec;
    }

    // A clock read rounds; a smaller difference is no suspend.
    constexpr std::int64_t kMinSleepSeconds = 2;

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

  ScreenActivityRecorder::ScreenActivityRecorder(wl_event_loop* loop, bool persist)
      : m_packageEnergy(loop), m_persist(persist), m_worker(loop) {
    if (m_persist) {
      m_log.parse(readFile(screenActivityPath()));
      m_energy.parse(readFile(appEnergyPath()));
    }
    const std::int64_t now = unixNow();
    m_lastInput = now;
    m_sleptSeconds = sleptSeconds();
    m_log.setScreenOn(true, now);
    writeLog(now);
    m_timer = wl_event_loop_add_timer(loop, onIdleCheck, this);
    if (m_timer != nullptr) {
      wl_event_source_timer_update(m_timer, kIdleCheckMs);
      m_timerArmed = true;
    }
  }

  ScreenActivityRecorder::~ScreenActivityRecorder() {
    m_closing = true;
    write(true);
    if (m_timer != nullptr) {
      wl_event_source_remove(m_timer);
    }
  }

  void ScreenActivityRecorder::checkSleep(std::int64_t now) {
    const std::int64_t slept = sleptSeconds();
    if (slept - m_sleptSeconds >= kMinSleepSeconds) {
      m_log.suspended(slept - m_sleptSeconds, now);
      m_sleptSeconds = slept;
      write(true);
    }
  }

  void ScreenActivityRecorder::setScreenOn(bool on) {
    const std::int64_t now = unixNow();
    checkSleep(now);
    m_log.setScreenOn(on, now);
    write(true);
    m_screenOn = on;
    reportIdle();
  }

  void ScreenActivityRecorder::reportIdle() {
    if (idle() != m_reportedIdle) {
      m_reportedIdle = idle();
      if (m_idleListener) {
        m_idleListener(m_reportedIdle);
      }
    }
  }

  void ScreenActivityRecorder::inputActivity() {
    const std::int64_t now = unixNow();
    // A suspend leaves a gap between inputs; within a stream of input the clocks are not read.
    if (now - m_lastInput >= kMinSleepSeconds) {
      checkSleep(now);
    }
    m_lastInput = now;
    if (m_idle) {
      m_idle = false;
      m_log.setUserIdle(false, now);
      writeLog(now);
      write(false);
      reportIdle();
    }
    if (!m_timerArmed && m_timer != nullptr) {
      wl_event_source_timer_update(m_timer, kIdleCheckMs);
      m_timerArmed = true;
    }
  }

  int ScreenActivityRecorder::onIdleCheck(void* data) {
    auto* self = static_cast<ScreenActivityRecorder*>(data);
    const std::int64_t now = unixNow();
    self->checkSleep(now);
    const std::int64_t quiet = now - self->m_lastInput;
    if (quiet >= ScreenActivityLog::kIdleAfter) {
      // The pause began at the last input, so idle time starts there.
      self->m_idle = true;
      self->m_log.setUserIdle(true, self->m_lastInput);
      self->m_timerArmed = false;
      self->writeLog(now);
      self->write(false);
      self->reportIdle();
    } else {
      wl_event_source_timer_update(self->m_timer, static_cast<int>((ScreenActivityLog::kIdleAfter - quiet) * 1000));
    }
    return 0;
  }

  // GPU time of every DRM client the user's processes hold, booked to its app unit (others only count in the total).
  void ScreenActivityRecorder::bookGpu(std::int64_t now) {
    using Booked = std::vector<std::pair<DrmClient, std::string>>;
    std::vector<std::pair<std::string, std::string>> apps; // unit, cgroup dir
    for (const auto& [unit, app] : qos::registry().apps) {
      apps.emplace_back(unit, app.dir);
    }
    auto scan = [apps = std::move(apps), manager = userManagerCgroup()]() {
      std::map<pid_t, std::string> unitOf;
      for (const auto& [unit, dir] : apps) {
        for (const pid_t pid : cgroupPids(dir)) {
          unitOf[pid] = unit;
        }
      }
      std::vector<pid_t> pids = manager.empty() ? std::vector<pid_t>{getpid()} : cgroupPids(manager);
      if (manager.empty()) {
        for (const auto& entry : unitOf) {
          pids.push_back(entry.first);
        }
      }
      Booked out;
      for (DrmClient& client : drmClients(pids)) {
        const auto unit = unitOf.find(client.pid);
        out.emplace_back(std::move(client), unit == unitOf.end() ? std::string() : unit->second);
      }
      return out;
    };
    auto book = [this, now](Booked booked) {
      std::set<std::string> live;
      for (const auto& [client, unit] : booked) {
        m_energy.bookGpu(client.key, unit, client.busyNs, now);
        live.insert(client.key);
      }
      m_energy.keepGpuClients(live);
      writeFile(appEnergyPath(), m_energy.serialize(now));
    };
    if (m_closing) {
      book(scan());
      return;
    }
    if (m_gpuScanBusy) {
      return;
    }
    m_gpuScanBusy = true;
    m_worker.post<Booked>(std::move(scan), [this, book = std::move(book)](Booked booked) {
      m_gpuScanBusy = false;
      book(std::move(booked));
    });
  }

  // The state file alone, on each idle change: Settings reads it and counts the running state on from there.
  void ScreenActivityRecorder::writeLog(std::int64_t now) {
    if (m_persist) {
      writeFile(screenActivityPath(), m_log.snapshot(now));
    }
  }

  void ScreenActivityRecorder::write(bool force) {
    const std::int64_t now = unixNow();
    if (!m_persist || (!force && now - m_writtenAt < kWriteEverySeconds)) {
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
    if (const auto all = readUsageUsec("/sys/fs/cgroup"); all.has_value()) {
      m_energy.bookTotal(*all, now);
    }
    writeFile(appEnergyPath(), m_energy.serialize(now));
    bookGpu(now);
    m_packageEnergy.request("PackageEnergyJ", [this, now](double joules) {
      if (std::isnan(joules)) {
        return;
      }
      m_energy.bookPackage(joules, now);
      writeFile(appEnergyPath(), m_energy.serialize(now));
      m_packageEnergy.request("UncoreEnergyJ", [this, now](double uncore) {
        if (!std::isnan(uncore)) {
          m_energy.bookUncore(uncore, now);
          writeFile(appEnergyPath(), m_energy.serialize(now));
        }
      });
    });
  }

} // namespace andromeda
