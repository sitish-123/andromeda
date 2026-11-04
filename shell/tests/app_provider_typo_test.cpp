#include "launcher/app_provider.h"
#include "system/desktop_entry.h"
#include "tests/test_check.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace {

  namespace fs = std::filesystem;

  void writeEntry(const fs::path& dir, std::string_view id, std::string_view name) {
    std::ofstream entry(dir / (std::string(id) + ".desktop"));
    entry << "[Desktop Entry]\nType=Application\nName=" << name << "\nExec=" << id << "\n";
  }

  bool contains(const std::vector<LauncherResult>& results, std::string_view title) {
    return std::ranges::find(results, title, &LauncherResult::title) != results.end();
  }

  std::vector<std::string> titles(const std::vector<LauncherResult>& results) {
    std::vector<std::string> out;
    for (const auto& r : results) {
      out.push_back(r.title);
    }
    return out;
  }

} // namespace

int main() {
  const fs::path root = fs::temp_directory_path() / ("noctalia-app-typo-" + std::to_string(getpid()));
  const fs::path applications = root / "data/applications";
  fs::create_directories(applications);
  writeEntry(applications, "probe-plimsoll", "Plimsoll Viewer");
  writeEntry(applications, "probe-quarnet", "Quarnet");
  writeEntry(applications, "probe-quarnix", "Quarnix");
  writeEntry(applications, "probe-quill", "Quill Zephyr");
  setenv("XDG_DATA_HOME", (root / "data").c_str(), 1);
  setenv("XDG_DATA_DIRS", (root / "empty").c_str(), 1);

  AppProvider provider(nullptr, nullptr);
  provider.initialize();

  // The real apps of the machine also answer, so only the probes' places are checked.
  // Swapped letters still find the app, from the start of a word in its name.
  TEST_CHECK(contains(provider.query("plismoll"), "Plimsoll Viewer"));
  // Short queries allow no typo ("lpi" is "pli" with two letters swapped): too many names are one letter away.
  TEST_CHECK(!contains(provider.query("lpi"), "Plimsoll Viewer"));
  // A real match ranks above a misspelt one: "quarnet" is Quarnet, and two letters off Quarnix.
  const auto quarnet = titles(provider.query("quarnet"));
  const auto exact = std::ranges::find(quarnet, "Quarnet");
  const auto typo = std::ranges::find(quarnet, "Quarnix");
  TEST_CHECK(exact != quarnet.end() && typo != quarnet.end() && exact < typo);
  // Initials find a multi-word name.
  TEST_CHECK(titles(provider.query("qz")).front() == "Quill Zephyr");

  fs::remove_all(root);
  return 0;
}
