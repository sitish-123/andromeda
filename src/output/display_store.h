#pragma once

#include <filesystem>
#include <span>
#include <string>
#include <string_view>

namespace umbriel {

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

  // Merges `outputs` into the [output] tables of the TOML document `existing`, replacing tables with the same name
  // and keeping the rest, so displays that are unplugged right now keep their saved state. Returns the new document.
  [[nodiscard]] std::string mergeSavedOutputs(std::string_view existing, std::span<const SavedOutput> outputs);

  // Merges `outputs` into `file` through a temporary file and a rename, so a config reload never reads a partial file.
  // Returns false on any I/O error.
  bool saveOutputs(const std::filesystem::path& file, std::span<const SavedOutput> outputs);

} // namespace umbriel
