#pragma once

#include "ui/files/file_browser.h"

#include <filesystem>
#include <optional>
#include <string>

// Per-folder view settings (FILES-UI-SPEC §16): the xattr user.andromeda.view on the folder, else one line per
// folder in $XDG_STATE_HOME/andromeda/files/views.tsv (filesystems without user xattrs, read-only folders).
// The value is a short key=value text the FileBrowser side encodes; this store only keeps it.
namespace view_store {
  [[nodiscard]] std::optional<std::string> read(const std::filesystem::path& folder);
  void write(const std::filesystem::path& folder, const std::string& value);
  // The text every folder without its own view uses ("Use as Defaults"); empty = built-in defaults.
  [[nodiscard]] std::string readDefault();
  void writeDefault(const std::string& value);

  // "v=list;z=64;s=added;d=1;h=0": unknown keys and bad values are skipped (a newer Files may write more keys).
  [[nodiscard]] std::string encode(const FileBrowserViewState& view);
  [[nodiscard]] FileBrowserViewState decode(const std::string& text);
  // The view a folder opens with: its own, else the smart default for its kind, else the user default.
  [[nodiscard]] FileBrowserViewState forFolder(const std::filesystem::path& folder);
} // namespace view_store
