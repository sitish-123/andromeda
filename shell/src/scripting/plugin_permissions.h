#pragma once

#include "core/toml.h" // IWYU pragma: keep

#include <filesystem>
#include <string>
#include <vector>

namespace scripting {

  // What a plugin may do outside its own folders (docs/plugin-permissions.md). Default: nothing.
  struct PluginPermissions {
    bool process = false;
    bool network = false;
    bool clipboardRead = false;
    bool environment = false;
    std::vector<std::string> filesRead;
    std::vector<std::string> filesWrite;

    bool operator==(const PluginPermissions&) const = default;
    // True when every permission in `requested` is also in this set.
    [[nodiscard]] bool covers(const PluginPermissions& requested) const;
    [[nodiscard]] bool empty() const;
  };

  // Reads the manifest's [permissions] table; a missing table means no permissions.
  [[nodiscard]] PluginPermissions parsePluginPermissions(const toml::table& root);

  // True when `path` lies inside one of `roots`. Both sides expand `~`, resolve `..` and resolve symlinks of the
  // parts that exist, so a link inside a root that points outside it does not pass.
  [[nodiscard]] bool pathWithin(const std::filesystem::path& path, const std::vector<std::filesystem::path>& roots);

  [[nodiscard]] std::vector<std::filesystem::path> expandRoots(const std::vector<std::string>& roots);

} // namespace scripting
