// Every settings section sits on exactly one sidebar page, and every legacy section id still opens a page.
#include "shell/settings/settings_pages.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

namespace {
  using namespace settings;

  void check(bool ok, const std::string& what) {
    if (!ok) {
      std::fprintf(stderr, "FAIL: %s\n", what.c_str());
      std::exit(1);
    }
  }
} // namespace

int main() {
  for (const auto& descriptor : settingsSectionDescriptors()) {
    int pages = 0;
    for (const auto& page : settingsPages()) {
      pages += settingsPageShowsSection(page, descriptor.section) ? 1 : 0;
    }
    check(pages == 1, std::string(descriptor.id) + " is on " + std::to_string(pages) + " pages");
    check(settingsPageFromId(descriptor.id) != nullptr, std::string(descriptor.id) + " does not open a page");
    check(
        settingsPageFromId(descriptor.id) == &settingsPageForSection(descriptor.section),
        std::string(descriptor.id) + " opens another page than the one that shows it"
    );
  }
  struct Legacy {
    std::string_view id;
    std::string_view page;
  };
  for (const Legacy legacy : {
           Legacy{"templates", "appearance"}, Legacy{"osd", "notifications"}, Legacy{"andromeda", "windows"},
           Legacy{"dock", "desktop-dock"}, Legacy{"desktop", "desktop-dock"}, Legacy{"panels", "control-center"},
           Legacy{"keybinds", "keyboard"}, Legacy{"shortcuts", "keyboard"}, Legacy{"input", "keyboard"},
           Legacy{"security", "privacy"}, Legacy{"location", "privacy"}, Legacy{"system", "about"},
           Legacy{"hooks", "automation"},
       }) {
    const auto* page = settingsPageFromId(legacy.id);
    check(page != nullptr && page->id == legacy.page, std::string(legacy.id) + " does not open " + std::string(legacy.page));
  }
  for (const std::string_view section : {"keybinds", "shortcuts", "input"}) {
    check(selectedPageShows("keyboard", section), "the keyboard page does not show " + std::string(section));
  }
  // Page reset and page filters use settingEntryOnPage: every section of a merged page must count as on that page.
  for (const auto& page : settingsPages()) {
    for (const auto section : page.sections) {
      SettingEntry entry;
      entry.section = section;
      entry.path = {section == SettingsSection::Dock ? "dock" : "x"};
      if (section == SettingsSection::Bar) {
        continue;
      }
      check(
          settingEntryOnPage(entry, page.id, "", ""),
          std::string(settingsSectionId(section)) + " is not on page " + std::string(page.id)
      );
    }
  }
  check(settingsPageFromId("unknown-page") == nullptr, "an unknown id opened a page");
  return 0;
}
