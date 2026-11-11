#include "service_menus.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <print>
#include <unistd.h>

namespace fs = std::filesystem;

int main() {
  char tmpl[] = "/tmp/files-servicemenus-XXXXXX";
  const fs::path root = ::mkdtemp(tmpl);
  ::setenv("XDG_DATA_HOME", (root / "home").c_str(), 1);
  ::setenv("XDG_DATA_DIRS", (root / "system").c_str(), 1);
  const fs::path menus = root / "home" / "kio" / "servicemenus";
  fs::create_directories(menus);
  std::ofstream(menus / "m.desktop") << "[Desktop Entry]\nType=Service\nMimeType=all/all;\nActions=a\n\n"
                                        "[Desktop Action a]\nName=Run\nExec=true %F\n";
  std::ofstream(root / "item.txt") << "x";
  int failures = 0;
  if (!service_menus::forItems({root / "item.txt"}).empty()) {
    std::println(stderr, "service_menus_test: a user menu without the exec bit loads");
    ++failures;
  }
  fs::permissions(menus / "m.desktop", fs::perms::owner_exec, fs::perm_options::add);
  if (service_menus::forItems({root / "item.txt"}).size() != 1) {
    std::println(stderr, "service_menus_test: an executable user menu does not load");
    ++failures;
  }
  fs::remove_all(root);
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
