#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <sdbus-c++/sdbus-c++.h>
#include <string>

class SessionBus;

// Serves org.freedesktop.impl.portal.Background under org.freedesktop.impl.portal.desktop.noctalia. xdg-desktop-portal
// asks which apps have windows (GetAppState) and, for a sandboxed app that runs with none, calls NotifyBackground.
// The reply is always "allow": the shell warns with its own notification, and Activity can stop the app. Autostart is
// written by xdg-desktop-portal itself (1.17+), so EnableAutostart only reports that it did nothing.
class BackgroundPortal {
public:
  // App ID -> 1 running (has a window) or 2 active (focused). Apps not in the map run in the background.
  using AppStates = std::map<std::string, std::uint32_t>;

  BackgroundPortal(
      SessionBus& bus, std::function<AppStates()> appStates,
      std::function<void(const std::string& appId, const std::string& name)> notifyBackground
  );
  ~BackgroundPortal();

  BackgroundPortal(const BackgroundPortal&) = delete;
  BackgroundPortal& operator=(const BackgroundPortal&) = delete;

  // Call on each toplevel change: emits RunningApplicationsChanged only when the app states changed.
  void onToplevelsChanged();

private:
  SessionBus& m_bus;
  std::function<AppStates()> m_appStates;
  std::function<void(const std::string&, const std::string&)> m_notifyBackground;
  std::unique_ptr<sdbus::IObject> m_object;
  AppStates m_lastStates;
};
