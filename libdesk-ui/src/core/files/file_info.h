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
  std::uint32_t mode = 0;
  std::uint64_t inode = 0;
};

// lstat + stat of one entry. Empty when it no longer exists.
[[nodiscard]] std::optional<FileInfo> statFile(int dirFd, std::string_view name);
[[nodiscard]] std::optional<FileInfo> statFile(const std::filesystem::path& path);
