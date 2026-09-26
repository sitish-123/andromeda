#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace umbriel {

  struct OutputIdentity;

  // One output as a settings client last applied it through zwlr_output_manager_v1.
  struct SavedOutput {
    // Descriptor ("<make> <model> <serial>") when the display reports EDID, else the connector.
    std::string name;
    bool enabled = true;
    int width = 0;
    int height = 0;
    int refreshMHz = 0;
    int x = 0;
    int y = 0;
    double scale = 1.0;
    int transform = 0; // wl_output_transform
    bool adaptiveSync = false;
  };

  // Name an output's saved table uses: its descriptor when the display reports EDID, else its connector.
  [[nodiscard]] std::string savedOutputName(const OutputIdentity& identity);

  // Merges `outputs` into the [output] tables of the TOML document `existing`, updating their keys and keeping every
  // other table and key (a saved mirror, displays that are unplugged right now). Returns the new document.
  [[nodiscard]] std::string mergeSavedOutputs(std::string_view existing, std::span<const SavedOutput> outputs);

  // Sets or, for nullopt, removes the `mirror` key of output `name`.
  [[nodiscard]] std::string
  mergeSavedMirror(std::string_view existing, const std::string& name, const std::optional<std::string>& source);

  // Merges `outputs` into `file` through a temporary file and a rename, so a config reload never reads a partial file.
  // Returns false on any I/O error.
  bool saveOutputs(const std::filesystem::path& file, std::span<const SavedOutput> outputs);
  bool saveMirror(const std::filesystem::path& file, const std::string& name, const std::optional<std::string>& source);

  // True when `configRoot` (the user's own config.toml, without its includes) has a non-empty [output.<name>] table,
  // which wins over displays.toml for that output. A damaged or missing file reads as false.
  [[nodiscard]] bool documentSetsOutput(const std::filesystem::path& configRoot, std::string_view name);

} // namespace umbriel
