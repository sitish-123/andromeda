#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// Reads the freedesktop trash (Trash spec 1.0): $XDG_DATA_HOME/Trash and the .Trash-$uid dirs of mounted volumes.
// Moving to and restoring from the trash is the FileOps1 daemon's job; the app only lists it.
struct TrashItem {
  std::filesystem::path trashDir;     // the Trash directory that holds it
  std::string name;                   // name under files/ and info/<name>.trashinfo
  std::filesystem::path originalPath; // absolute, decoded from Path=
  std::string deletionDate;           // DeletionDate= as written (local time, no zone)
  bool isDir = false;
  std::uint64_t size = 0;
};

[[nodiscard]] std::filesystem::path homeTrashDir();
// The home trash first, then one per mounted volume that has a trash of this user.
[[nodiscard]] std::vector<std::filesystem::path> trashDirs();
// Items with a readable info file; an orphan in files/ without info is skipped.
[[nodiscard]] std::vector<TrashItem> listTrash(const std::filesystem::path& trashDir);
// Path= value: percent-decoded; relative paths are relative to the volume that holds a volume trash.
[[nodiscard]] std::filesystem::path decodeTrashPath(const std::string& value, const std::filesystem::path& trashDir);
