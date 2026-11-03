#pragma once

#include "dbus/systemd/systemd_units_service.h"
#include "system/xdg_autostart.h"

#include <functional>
#include <map>
#include <memory>
#include <string>

namespace sdbus {
  class IConnection;
  class IProxy;
} // namespace sdbus

struct ActivityActionResult {
  bool ok = false;
  bool accessDenied = false; // polkit said no or the user cancelled: "You need administrator rights."
  // Disable only: a socket or D-Bus activation can still start the unit.
  bool stillStartsOnDemand = false;
  std::string error;
};

// Writes or removes the user's Hidden=true override for an autostart entry. An override the shell created carries
// X-Andromeda-Override=true and is deleted on undo; the user's own file only gains or loses its Hidden= line.
bool setAutostartHidden(const std::string& userDir, const AutostartEntry& entry, bool hidden, std::string& error);

// Stop and Disable at login for Activity rows. User units go to the user manager; system units go through
// polkit (org.freedesktop.systemd1.manage-units / manage-unit-files) with the shell's agent.
class ActivityActions {
public:
  using Reply = std::function<void(ActivityActionResult)>;

  // systemBus may be null: system-unit reads (on-demand check) then report false.
  ActivityActions(sdbus::IConnection& userBus, sdbus::IConnection* systemBus);
  ~ActivityActions();

  ActivityActions(const ActivityActions&) = delete;
  ActivityActions& operator=(const ActivityActions&) = delete;

  void stop(const SystemdUnit& unit, Reply reply);
  // An autostart unit (app-*@autostart.service) gets the XDG override in autostartUserDir; any other unit gets
  // DisableUnitFiles, which keeps the unit file and removes only its [Install] links.
  void disableAtLogin(const SystemdUnit& unit, const std::vector<AutostartEntry>& autostart, const std::string& autostartUserDir, Reply reply);
  // Undo of disableAtLogin.
  void enableAtLogin(const SystemdUnit& unit, const std::vector<AutostartEntry>& autostart, const std::string& autostartUserDir, Reply reply);

private:
  void checkOnDemand(const SystemdUnit& unit, ActivityActionResult result, Reply reply);
  sdbus::IProxy& proxyFor(const SystemdUnit& unit, const std::string& path);

  sdbus::IConnection& m_userBus;
  sdbus::IConnection* m_systemBus;
  std::map<std::string, std::unique_ptr<sdbus::IProxy>> m_proxies; // by "u:" or "s:" + object path
};
