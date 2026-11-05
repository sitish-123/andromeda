#pragma once

#include "core/timer_manager.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class SessionBus;
class SystemBus;

namespace sdbus {
  class IProxy;
}

struct SoftwareUpdate {
  // system, apps or firmware.
  std::string group;
  std::string name;
  // Empty when the source does not say (Flatpak).
  std::string oldVersion;
  std::string newVersion;
  std::uint64_t sizeBytes = 0;
};

struct SoftwareUpdateNews {
  std::string title;
  std::string link;
  std::int64_t published = 0;
};

// Client of org.andromeda.Update1 (session: what is available, Arch news) and org.andromeda.UpdateHelper1 (system:
// the upgrade itself, polkit-gated). docs/system.md §1.
class SoftwareUpdateService {
public:
  enum class RunState : std::uint8_t { Idle, Running, Done, NeedsChoice, Failed };

  SoftwareUpdateService(SessionBus& session, SystemBus* system);
  ~SoftwareUpdateService();

  void setChangeCallback(std::function<void()> callback) { m_changeCallback = std::move(callback); }

  [[nodiscard]] bool available() const noexcept { return m_available; }
  [[nodiscard]] const std::vector<SoftwareUpdate>& updates() const noexcept { return m_updates; }
  // Unread Arch news, newest first.
  [[nodiscard]] const std::vector<SoftwareUpdateNews>& news() const noexcept { return m_news; }
  [[nodiscard]] bool newsBlocksSystem() const noexcept { return m_newsBlocksSystem; }
  [[nodiscard]] std::int64_t lastCheck() const noexcept { return m_lastCheck; }
  [[nodiscard]] bool checking() const noexcept { return m_checking; }
  // One line per source that failed in the last check.
  [[nodiscard]] const std::vector<std::string>& errors() const noexcept { return m_errors; }

  [[nodiscard]] RunState runState() const noexcept { return m_runState; }
  [[nodiscard]] const std::string& stage() const noexcept { return m_stage; }
  [[nodiscard]] const std::string& stagePackage() const noexcept { return m_stagePackage; }
  // 0-100 within the current stage.
  [[nodiscard]] int percent() const noexcept { return m_percent; }
  [[nodiscard]] const std::string& resultMessage() const noexcept { return m_resultMessage; }
  [[nodiscard]] const std::vector<std::string>& questions() const noexcept { return m_questions; }
  // A kernel, Mesa, systemd or glibc update was installed: the session must restart to use it.
  [[nodiscard]] bool restartNeeded() const noexcept { return m_restartNeeded; }

  // Reads the session service (D-Bus activates it); the page calls this when it opens.
  void refresh();
  void check();
  // False without a system bus: nothing can be installed or marked read from here.
  [[nodiscard]] bool canInstall() const noexcept { return m_helper != nullptr; }
  // Marks news up to the newest unread item as read, in the session service and in the helper (polkit).
  void markNewsRead();
  // System (helper, root) and Apps (flatpak, as the user) together.
  void upgrade();
  // Update lock: `then(waited)` runs at once when no system update runs, else after it finishes (at most 60 min).
  // `waiting` runs when the wait starts. One pending action: a later call replaces it. Does not start the helper.
  void afterSystemUpdate(std::function<void(bool waited)> then, std::function<void()> waiting);
  [[nodiscard]] bool appsUpdating() const noexcept { return m_appsUpdating; }
  // The last line flatpak printed when its update failed; empty after a success.
  [[nodiscard]] const std::string& appsError() const noexcept { return m_appsError; }
  // Tonight's 02:00 run (on AC, all sessions locked): the user authenticates now; false cancels.
  void scheduleTonight(bool enable);
  [[nodiscard]] bool tonight() const noexcept { return m_tonight; }
  // What snapshots the system before an update: "snap-pac", "snapper" or empty.
  [[nodiscard]] const std::string& snapshots() const noexcept { return m_snapshots; }

private:
  void load();
  void notify();

  SessionBus& m_session;
  SystemBus* m_system = nullptr;
  std::unique_ptr<sdbus::IProxy> m_update;
  std::unique_ptr<sdbus::IProxy> m_helper;
  std::unique_ptr<sdbus::IProxy> m_busDaemon;
  std::shared_ptr<int> m_lifetimeToken = std::make_shared<int>(0);
  std::function<void()> m_changeCallback;

  bool m_available = false;
  std::vector<SoftwareUpdate> m_updates;
  std::vector<SoftwareUpdateNews> m_news;
  bool m_newsBlocksSystem = false;
  std::int64_t m_lastCheck = 0;
  bool m_checking = false;
  std::vector<std::string> m_errors;

  RunState m_runState = RunState::Idle;
  std::string m_stage;
  std::string m_stagePackage;
  int m_percent = 0;
  std::string m_resultMessage;
  std::vector<std::string> m_questions;
  bool m_restartNeeded = false;
  bool m_tonight = false;
  std::string m_snapshots;
  std::function<void(bool)> m_updateWaiter;
  Timer m_updateWaitCap;
  void releaseWaiters();
  bool m_appsUpdating = false;
  std::string m_appsError;

  void upgradeApps();
};
