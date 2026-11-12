#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace andromeda {

  struct OutputIdentity;

  struct SavedMode {
    int width = 0;
    int height = 0;
    int refreshMHz = 0;

    bool operator==(const SavedMode&) const = default;
  };

  // One output as a settings client applied it through zwlr_output_manager_v1. Only the keys that are set are written.
  struct SavedOutput {
    // Descriptor ("<make> <model> <serial>") when the display reports EDID, else the connector.
    std::string name;
    std::optional<bool> enabled;
    std::optional<SavedMode> mode;
    // Unset for a mirror, which keeps the position it last had on the desktop.
    std::optional<std::array<int, 2>> position;
    std::optional<double> scale;
    std::optional<int> transform; // wl_output_transform
    std::optional<bool> adaptiveSync;

    bool operator==(const SavedOutput&) const = default;
  };

  // The keys of `requested` whose value differs from `live`, so an apply saves only what its request changed and never
  // a value the compositor chose (a capped mode, an automatic scale). The result keeps the name of `requested`.
  [[nodiscard]] SavedOutput changedOutputKeys(const SavedOutput& live, const SavedOutput& requested);

  // Unsets the key of `output` that the [output] config key `key` stores ("mode", "vrr").
  void dropSavedKey(SavedOutput& output, std::string_view key);

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

  // A display property a settings app sets beyond what zwlr_output_manager_v1 carries ("hdr", "workspaces");
  // std::monostate removes the key.
  using SavedProperty = std::variant<std::monostate, bool, int64_t, double, std::string>;

  // Sets or removes one key of output `name`'s saved table in `file`. Returns false on any I/O error.
  bool saveOutputProperty(
      const std::filesystem::path& file, const std::string& name, std::string_view key, const SavedProperty& value
  );

  // True when `configRoot` (the user's own config.toml, without its includes) sets `key` in its [output.<name>] table.
  // That key wins over displays.toml; the table's other keys come from displays.toml. A damaged or missing file reads
  // as false.
  [[nodiscard]] bool
  documentSetsOutputKey(const std::filesystem::path& configRoot, std::string_view name, std::string_view key);
  // The keys `configRoot` sets in its [output.<name>] table; empty for a damaged or missing file.
  [[nodiscard]] std::vector<std::string>
  documentOutputKeys(const std::filesystem::path& configRoot, std::string_view name);

} // namespace andromeda
