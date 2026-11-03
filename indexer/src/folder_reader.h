#pragma once

#include <sys/types.h>

#include <cstdint>
#include <string>
#include <vector>

namespace andromeda::index {

  struct DiskEntry {
    std::string name;
    std::int64_t mtime = 0; // nanoseconds since the epoch
    std::int64_t size = 0;  // 0 for folders
    bool isDir = false;
  };

  enum class FolderRead : std::uint8_t {
    Ok,
    Skipped, // holds a marker (.git, .nomedia, ...): nothing below it is indexed
    Missing, // gone or unreadable
  };

  // Reads one folder with every skip rule applied: hidden entries, generated folders, markers (ignored when
  // isRoot), symlinks, special files and folders on another filesystem than `device`.
  FolderRead readFolder(const std::string& path, bool isRoot, dev_t device, std::vector<DiskEntry>& out);

  // Modification time of a path in nanoseconds, or nullopt-like -1 when it cannot be read.
  [[nodiscard]] std::int64_t mtimeOf(const std::string& path);

} // namespace andromeda::index
