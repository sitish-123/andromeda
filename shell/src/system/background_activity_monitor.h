#pragma once

#include "core/timer_manager.h"
#include "system/background_activity.h"

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

class ActivityMonitor;
class ActivityActions;
class BackgroundWarningState;
class NotificationManager;
struct SystemdUnit;

// The background warning: 10 min after login and then every 30 min it lists units once (no live sampling),
// classifies them and posts one notification per new finding, or one grouped notification for 3 or more.
class BackgroundActivityMonitor {
public:
  struct Hooks {
    std::function<std::vector<int>()> trayPids;               // PIDs that own a StatusNotifierItem
    std::function<std::set<ShellFeature>()> enabledFeatures; // shell features that make helpers redundant
    std::function<void(const std::string& rowKey)> showActivity;
    std::function<bool()> enabled; // activity.background.warn
  };

  BackgroundActivityMonitor(
      ActivityMonitor& activity, ActivityActions& actions, BackgroundWarningState& state, NotificationManager& notifications,
      Hooks hooks
  );

  void start();
  // Runs one check now (IPC `activity-check`); done() runs after any notification is posted.
  void checkNow(std::function<void(std::size_t findings)> done = {});

  static constexpr auto kFirstCheck = std::chrono::minutes(10);
  static constexpr auto kInterval = std::chrono::minutes(30);

private:
  struct Finding {
    std::string rowKey;
    std::string name;
    std::string unitKey; // FragmentPath, else unit name
    std::int64_t mtime = 0;
    BackgroundVerdict verdict;
    std::uint64_t memoryBytes = 0;
    bool atLogin = false;
    std::vector<SystemdUnit> units;
  };

  std::vector<Finding> evaluate() const;
  void notify(std::vector<Finding> findings);
  void onAction(std::uint32_t id, const std::string& action);

  ActivityMonitor& m_activity;
  ActivityActions& m_actions;
  BackgroundWarningState& m_state;
  NotificationManager& m_notifications;
  Hooks m_hooks;
  Timer m_timer;
  bool m_started = false;
  std::map<std::uint32_t, std::vector<Finding>> m_posted; // notification id -> its findings
};
