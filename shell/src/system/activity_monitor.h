#pragma once

#include "dbus/systemd/systemd_units_service.h"
#include "system/activity_service.h"
#include "core/timer_manager.h"

#include <functional>
#include <string>
#include <utility>
#include <vector>

class SessionBus;

// Feeds ActivityService from the systemd user manager, compositor window PIDs and cgroup files. Samples every
// second only while a reference or a touch() lease is held; with none, it holds no timer and no D-Bus subscription.
class ActivityMonitor {
public:
  // PID and app_id of every mapped window.
  using WindowPids = std::function<std::vector<std::pair<int, std::string>>()>;
  using ChangeCallback = std::function<void()>;

  ActivityMonitor(SessionBus& bus, WindowPids windowPids, std::string cgroupRoot, ChangeCallback onChange = {});
  ~ActivityMonitor();

  ActivityMonitor(const ActivityMonitor&) = delete;
  ActivityMonitor& operator=(const ActivityMonitor&) = delete;

  // Called after every sample and every unit-list change; one listener (the Activity window).
  void setChangeCallback(ChangeCallback onChange) { m_onChange = std::move(onChange); }

  void retain();
  void release();
  // Samples for 10 s, for readers with no lifetime to tie a reference to.
  void touch();
  // One unit list and one cgroup read, then done(); no timer, no D-Bus subscription. For the background check.
  void sampleOnce(std::function<void()> done);

  [[nodiscard]] bool sampling() const noexcept { return m_sampling; }
  [[nodiscard]] const ActivityService& activity() const noexcept { return m_activity; }
  [[nodiscard]] const SystemdUnitsService& userUnits() const noexcept { return m_userUnits; }
  // Rows as a JSON array, for `msg activity-dump`.
  [[nodiscard]] std::string dumpJson() const;
private:
  // Maps window PIDs to units; a few /proc reads, done with every sample.
  void windowsChanged();
  void updateSampling();
  void sampleNow();

  WindowPids m_windowPids;
  ChangeCallback m_onChange;
  SystemdUnitsService m_userUnits;
  ActivityService m_activity;
  Timer m_sampleTimer;
  Timer m_leaseTimer;
  int m_refs = 0;
  bool m_leased = false;
  bool m_sampling = false;
  std::vector<std::function<void()>> m_onceWaiters;
};
