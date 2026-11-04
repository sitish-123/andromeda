#pragma once

#include <string>
#include <string_view>
#include <vector>

// One XDG autostart entry after the user-over-system lookup (autostart spec: the first directory wins by file name).
struct AutostartEntry {
  std::string id; // file name without ".desktop"
  std::string path;
  std::string name;
  std::string exec;
  std::string icon;
  // The unit systemd-xdg-autostart-generator makes for this entry: app-<escaped id>@autostart.service.
  std::string unitName;
  // False when the generator makes no unit (Hidden, X-systemd-skip, X-GNOME-Autostart-enabled=false, TryExec
  // missing) or OnlyShowIn/NotShowIn exclude every current desktop.
  bool startsHere = true;
  bool userFile = false; // path is in the user directory (an override of a system file or a user entry)
};

struct AutostartDirs {
  std::string user;                // $XDG_CONFIG_HOME/autostart
  std::vector<std::string> system; // $XDG_CONFIG_DIRS/*/autostart, most important first
};

[[nodiscard]] AutostartDirs autostartDirsFromEnv();

// currentDesktops is $XDG_CURRENT_DESKTOP (colon-separated).
[[nodiscard]] std::vector<AutostartEntry> scanAutostart(const AutostartDirs& dirs, std::string_view currentDesktops);
