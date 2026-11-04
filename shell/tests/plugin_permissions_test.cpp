#include "core/toml.h"
#include "scripting/plugin_permissions.h"
#include "tests/test_check.h"

#include <cstdlib>
#include <filesystem>

using scripting::PluginPermissions;

int main() {
  const auto dir = std::filesystem::temp_directory_path() / "plugin_permissions_test";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir / "allowed" / "sub");
  std::filesystem::create_directories(dir / "secret");
  std::filesystem::create_symlink(dir / "secret", dir / "allowed" / "escape");
  const std::vector<std::filesystem::path> roots{dir / "allowed"};

  TEST_CHECK(scripting::pathWithin(dir / "allowed" / "sub" / "new.txt", roots));
  TEST_CHECK(scripting::pathWithin(dir / "allowed", roots));
  TEST_CHECK(!scripting::pathWithin(dir / "allowed" / ".." / "secret" / "key", roots));
  TEST_CHECK(!scripting::pathWithin(dir / "allowed" / "escape" / "key", roots));
  TEST_CHECK(!scripting::pathWithin(dir / "allowedx" / "file", roots));
  TEST_CHECK(!scripting::pathWithin("relative/file", roots));

  const auto parsed = toml::parse(R"(
[permissions]
process = true
files_read = ["~/Pictures"]
)");
  const PluginPermissions requested = scripting::parsePluginPermissions(parsed);
  TEST_CHECK(requested.process && !requested.network && requested.filesRead.size() == 1);
  TEST_CHECK(scripting::parsePluginPermissions(toml::parse("name = \"x\"")).empty());

  PluginPermissions granted;
  TEST_CHECK(!granted.covers(requested));
  granted.process = true;
  granted.filesRead = {"~"};
  TEST_CHECK(granted.covers(requested));
  granted.filesRead = {"~/Documents"};
  TEST_CHECK(!granted.covers(requested));

  std::filesystem::remove_all(dir);
  return EXIT_SUCCESS;
}
