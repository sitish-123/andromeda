#pragma once

#include <filesystem>
#include <vector>

// Starred files and folders (GNOME Files' Starred, Ctrl+D): $XDG_DATA_HOME/andromeda/files/starred.json, a list of
// absolute paths, newest first. A JSON list instead of an xattr, so it works on every file system and on mounts.
class Starred {
public:
  Starred();

  [[nodiscard]] const std::vector<std::filesystem::path>& paths() const noexcept { return m_paths; }
  [[nodiscard]] bool contains(const std::filesystem::path& path) const;
  // Stars every path when one of them is not starred, else unstars them all; true = they are starred now.
  bool toggle(const std::vector<std::filesystem::path>& paths);
  // Follows a rename or move done in Files. Missing paths stay: they may be on a drive that is not mounted now.
  void moved(const std::filesystem::path& from, const std::filesystem::path& to);

  [[nodiscard]] static std::filesystem::path storePath();

private:
  void save() const;

  std::vector<std::filesystem::path> m_paths;
};
