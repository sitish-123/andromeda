#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

struct DesktopEntry;

namespace shell::dock {

  struct DockInstance;

  // File drops on the dock (FILES-UI-SPEC §20): a drop on an app opens the files with it, as on the macOS Dock.
  // The app item under a surface point (the launcher button is not an item).
  [[nodiscard]] std::optional<std::size_t> itemAt(const DockInstance& instance, float x, float y);
  // True when the app's Exec takes files or URIs (%f %F %u %U).
  [[nodiscard]] bool takesFiles(const DesktopEntry& entry);
  // The Exec lines that open a dropped text/uri-list (Desktop Entry spec): %F/%U get every item, %f/%u one item per
  // line; %f/%F take local files only. Empty when nothing fits. The lines still go through desktop_entry_launch.
  [[nodiscard]] std::vector<std::string> execLines(const std::string& exec, const std::string& uriList);

} // namespace shell::dock
