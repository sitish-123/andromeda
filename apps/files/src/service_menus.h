#pragma once

#include <filesystem>
#include <string>
#include <vector>

// KDE ServiceMenus (FILES-UI-SPEC §16): .desktop files in kio/servicemenus and andromeda-files/servicemenus under
// the user and system data dirs. Each [Desktop Action] whose MimeType covers every selected item is an "Actions ›"
// row in the item menu.
namespace service_menus {
  struct Action {
    std::string name;
    std::string icon;
    std::string exec;    // the Exec line, field codes not expanded
    std::string submenu; // X-KDE-Submenu, empty = top level
  };

  // The actions for these items, in file then action order; files are read on each call (a menu opens rarely).
  [[nodiscard]] std::vector<Action> forItems(const std::vector<std::filesystem::path>& paths);
  // Expands %f %F %u %U and runs the command in `folder`; false when the line does not parse or spawn fails.
  bool run(const Action& action, const std::vector<std::filesystem::path>& paths, const std::filesystem::path& folder);
} // namespace service_menus
