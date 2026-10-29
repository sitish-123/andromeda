#pragma once

#include <functional>
#include <memory>
#include <sdbus-c++/sdbus-c++.h>

class SessionBus;
class NotificationManager;

// org.andromeda.Shell1: what Andromeda Link's link-session needs from the shell. Do Not Disturb, read and set, with
// PropertiesChanged; ScreenOff().
class AndromedaShellService {
public:
  AndromedaShellService(SessionBus& bus, NotificationManager& notifications, std::function<bool()> screenOff);

  // Call after any notification state change; signals only a Do Not Disturb change.
  void notificationStateChanged();

private:
  NotificationManager& m_notifications;
  std::function<bool()> m_screenOff;
  std::unique_ptr<sdbus::IObject> m_object;
  bool m_doNotDisturb;
};
