#pragma once

#include <functional>
#include <memory>
#include "shell/lockscreen/lock_notice_groups.h"

#include <sdbus-c++/sdbus-c++.h>
#include <vector>

class SessionBus;
class NotificationManager;

// org.andromeda.Shell1: what Andromeda Link's link-session needs from the shell. Do Not Disturb, read and set, with
// PropertiesChanged; ScreenOff(); LockFromPhone(). For the `noctalia lock` process: LockNotices (read-only, the
// unseen notification groups the lock screen shows, no content; lock_notice_wire.h), with PropertiesChanged.
class AndromedaShellService {
public:
  AndromedaShellService(
      SessionBus& bus, NotificationManager& notifications, std::function<bool()> screenOff,
      std::function<bool()> lockFromPhone
  );
  ~AndromedaShellService();
  AndromedaShellService(const AndromedaShellService&) = delete;
  AndromedaShellService& operator=(const AndromedaShellService&) = delete;

  // Call after any notification state change; signals only a Do Not Disturb change.
  void notificationStateChanged();

private:
  void scheduleLockNotices();

  NotificationManager& m_notifications;
  std::function<bool()> m_screenOff;
  std::function<bool()> m_lockFromPhone;
  std::unique_ptr<sdbus::IObject> m_object;
  bool m_doNotDisturb;
  std::vector<LockNoticeGroup> m_lockNotices;
  bool m_lockNoticesPending = false;
  int m_eventToken = 0;
  int m_unreadToken = 0;
  std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};
