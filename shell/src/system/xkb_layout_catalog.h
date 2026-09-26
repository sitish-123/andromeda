#pragma once

#include <string>
#include <vector>

// Reads the XKB rules registry (/usr/share/X11/xkb/rules/evdev.xml) for the Keyboard settings
// page: layouts, their variants, and the option groups (Caps Lock behavior, Compose key, ...).
namespace xkb {

  struct LayoutVariant {
    std::string name;
    std::string description;
  };

  struct Layout {
    std::string name;
    std::string description;
    std::vector<LayoutVariant> variants;
  };

  struct OptionEntry {
    std::string name; // full "group:option" XKB option string
    std::string description;
  };

  struct OptionGroup {
    std::string name; // e.g. "caps", "compose"
    std::string description;
    bool allowMultiple = false;
    std::vector<OptionEntry> options;
  };

  struct Catalog {
    std::vector<Layout> layouts;
    std::vector<OptionGroup> optionGroups;
  };

  // Parses the rules file at `path` (defaults to the system evdev registry). Returns an empty
  // catalog, logging a warning, when the file is missing or malformed.
  Catalog loadCatalog(const std::string& path = "/usr/share/X11/xkb/rules/evdev.xml");

  // Convenience lookups over a loaded catalog.
  const Layout* findLayout(const Catalog& catalog, std::string_view name);
  const OptionGroup* findOptionGroup(const Catalog& catalog, std::string_view name);

} // namespace xkb
