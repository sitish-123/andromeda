#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

enum class FileKind : std::uint8_t { File, Directory, Symlink, Other };

struct FileInfo {
  std::string name;              // file name bytes as on disk (not always UTF-8)
  std::string displayName;       // valid UTF-8 for labels
  std::string collateKey;        // natural, locale-aware order: "file2" before "file10"
  std::string contentType;       // from the name only, e.g. "image/png", "inode/directory"
  FileKind kind = FileKind::File;
  bool isDir = false;            // a directory, or a symlink to one
  bool hidden = false;           // a dot file
  std::uint64_t size = 0;        // bytes; 0 for directories
  std::int64_t mtimeNs = 0;      // CLOCK_REALTIME ns
  std::int64_t addedNs = 0;      // when the entry landed here: birth time, or the change time without one
  std::uint32_t mode = 0;
  std::uint64_t inode = 0;
  bool pendingStat = false;      // listed by name only: size, mtimeNs, mode and inode are not read yet
};

// lstat + stat of one entry. Empty when it no longer exists.
[[nodiscard]] std::optional<FileInfo> statFile(int dirFd, std::string_view name);
[[nodiscard]] std::optional<FileInfo> statFile(const std::filesystem::path& path);
// An entry from its directory record: no stat when dType names a file or directory (pendingStat), else statFile.
[[nodiscard]] std::optional<FileInfo> fileFromDirent(int dirFd, std::string_view name, unsigned char dType);
// Reads the stat fields of info.name and clears pendingStat. False when it no longer exists.
bool fillStat(int dirFd, FileInfo& info);
