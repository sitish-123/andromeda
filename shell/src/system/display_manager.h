#pragma once

#include <functional>
#include <string>
#include <vector>

// The system login screen: which display managers are installed, which one display-manager.service points at, and
// the polkit switch (systemctl enable -f) between them. The change applies at the next start.
namespace display_manager {

  struct Entry {
    std::string unit; // "greetd.service"
    std::string name; // "Andromeda", "GDM"
  };

  // greetd counts only with the Andromeda greeter's config; others when their unit file exists.
  [[nodiscard]] std::vector<Entry> installed();
  // Unit display-manager.service links to; empty without one.
  [[nodiscard]] std::string activeUnit();
  [[nodiscard]] std::string nameOf(const std::string& unit);
  // EnableUnitFiles(unit, force) through the shell's polkit agent. `denied`: polkit refused or the user cancelled.
  void switchTo(const std::string& unit, std::function<void(bool ok, bool denied, std::string error)> reply);

} // namespace display_manager
