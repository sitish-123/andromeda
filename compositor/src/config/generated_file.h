#pragma once

#include "core/toml.h"

#include <filesystem>
#include <functional>
#include <initializer_list>
#include <string>
#include <string_view>

namespace umbriel {

  // A TOML file Umbriel writes on the user's behalf (displays.toml, settings.toml).

  // Parses `existing` (a damaged document starts empty), applies `edit`, and serializes it under `header`.
  [[nodiscard]] std::string
  editGeneratedToml(std::string_view existing, std::string_view header, const std::function<void(toml::table&)>& edit);

  // editGeneratedToml on `file`, written through a temporary file and a rename so a config reload never reads a
  // partial file. Returns false on any I/O error.
  bool rewriteGeneratedToml(
      const std::filesystem::path& file, std::string_view header, const std::function<void(toml::table&)>& edit
  );

  // The table at `path` below `root`, created along the way. A non-table value in the way is replaced.
  toml::table& generatedTable(toml::table& root, std::initializer_list<std::string_view> path);

} // namespace umbriel
