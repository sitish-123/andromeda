#pragma once

#include "core/timer_manager.h"

#include <sdbus-c++/sdbus-c++.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

class IpcService;
class SoftwareUpdateService;
class SystemBus;

// What stands in the way of ending the session (.agent/DESIGN.md "Power menu and session end"): logind block
// inhibitors on shutdown (pacman, firmware, format) and other users' sessions. Restart and Shut Down wait behind a block
// inhibitor ("When done") and run once it is gone; Update and Restart waits for the update to finish. logind sends no
// change signal for inhibitors, so they are read every 2 s, and only while someone watches or an action waits.
class SessionGuard {
public:
  struct Blocker {
    std::string who;
    std::string why;
    bool operator==(const Blocker&) const = default;
  };

  void initialize(SystemBus* bus, IpcService* ipc);
  void setChangeCallback(std::function<void()> callback) { m_changeCallback = std::move(callback); }
  // Counted: the power menu and the session-end alert each watch while they show.
  void watch(bool on);

  [[nodiscard]] const std::vector<Blocker>& blockers() const noexcept { return m_blockers; }
  [[nodiscard]] const std::vector<std::string>& otherUsers() const noexcept { return m_otherUsers; }
  // logind can boot into the firmware setup next time (UEFI, CanRebootToFirmwareSetup yes or challenge).
  [[nodiscard]] bool firmwareSetup() const noexcept { return m_firmwareSetup; }

  // `action` is reboot or shutdown; it runs through the session end once no block inhibitor is left.
  void queue(const std::string& action);
  // Installs the waiting updates, then restarts through the session end.
  void updateAndRestart(SoftwareUpdateService& updates);
  void cancel();
  [[nodiscard]] const std::string& queued() const noexcept { return m_queued; }
  [[nodiscard]] bool updating() const noexcept { return m_updates != nullptr; }
  void onUpdateChanged();

private:
  void refresh();
  void syncTimer();
  void runQueued();
  void notify();

  std::unique_ptr<sdbus::IProxy> m_logind;
  IpcService* m_ipc = nullptr;
  std::function<void()> m_changeCallback;
  Timer m_poll;
  int m_watchers = 0;
  std::vector<Blocker> m_blockers;
  std::vector<std::string> m_otherUsers;
  std::string m_queued;
  SoftwareUpdateService* m_updates = nullptr;
  bool m_firmwareSetup = false;
  std::shared_ptr<int> m_alive = std::make_shared<int>(0);
};
