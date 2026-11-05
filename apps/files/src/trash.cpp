#include "trash.h"

#include <algorithm>

#include <cstdlib>
#include <fstream>
#include <mntent.h>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {
  int hexValue(char c) {
    if (c >= '0' && c <= '9') {
      return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
      return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
      return c - 'A' + 10;
    }
    return -1;
  }

  // A volume trash is valid only as a non-symlink directory with the sticky bit (spec, "Trash directories").
  bool validSharedTrash(const fs::path& dir) {
    struct stat st{};
    return ::lstat(dir.c_str(), &st) == 0 && S_ISDIR(st.st_mode) && (st.st_mode & S_ISVTX) != 0;
  }
} // namespace

fs::path homeTrashDir() {
  if (const char* data = std::getenv("XDG_DATA_HOME"); data != nullptr && data[0] == '/') {
    return fs::path(data) / "Trash";
  }
  const char* home = std::getenv("HOME");
  return fs::path(home != nullptr ? home : "/") / ".local/share/Trash";
}

std::vector<fs::path> trashDirs() {
  std::vector<fs::path> dirs{homeTrashDir()};
  const std::string uid = std::to_string(::getuid());
  FILE* mounts = ::setmntent("/proc/self/mounts", "r");
  if (mounts == nullptr) {
    return dirs;
  }
  std::error_code ec;
  while (const mntent* m = ::getmntent(mounts)) {
    const fs::path top = m->mnt_dir;
    if (top.string().starts_with("/proc") || top.string().starts_with("/sys")
        || top.string().starts_with("/dev") || top.string().starts_with("/run/user")) {
      continue;
    }
    if (const fs::path shared = top / ".Trash"; validSharedTrash(shared) && fs::is_directory(shared / uid, ec)) {
      dirs.push_back(shared / uid);
    }
    if (const fs::path own = top / (".Trash-" + uid); fs::is_directory(own, ec)) {
      dirs.push_back(own);
    }
  }
  ::endmntent(mounts);
  // Bind and stacked mounts show one trash directory under several paths.
  std::vector<fs::path> unique;
  std::vector<std::pair<dev_t, ino_t>> seen;
  for (fs::path& dir : dirs) {
    struct stat st {};
    if (::stat(dir.c_str(), &st) == 0) {
      const std::pair id{st.st_dev, st.st_ino};
      if (std::ranges::contains(seen, id)) {
        continue;
      }
      seen.push_back(id);
    }
    unique.push_back(std::move(dir));
  }
  return unique;
}

fs::path decodeTrashPath(const std::string& value, const fs::path& trashDir) {
  std::string out;
  out.reserve(value.size());
  for (std::size_t i = 0; i < value.size(); ++i) {
    if (value[i] == '%' && i + 2 < value.size()) {
      const int hi = hexValue(value[i + 1]);
      const int lo = hexValue(value[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out.push_back(static_cast<char>(hi * 16 + lo));
        i += 2;
        continue;
      }
    }
    out.push_back(value[i]);
  }
  fs::path path = out;
  if (path.is_relative()) {
    // A volume trash is <top>/.Trash/<uid> or <top>/.Trash-<uid>.
    const fs::path top = trashDir.parent_path().filename() == ".Trash" ? trashDir.parent_path().parent_path()
                                                                       : trashDir.parent_path();
    path = top / path;
  }
  return path.lexically_normal();
}

std::vector<TrashItem> listTrash(const fs::path& trashDir) {
  std::vector<TrashItem> items;
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator(trashDir / "info", ec)) {
    const fs::path info = entry.path();
    if (info.extension() != ".trashinfo") {
      continue;
    }
    std::ifstream in(info);
    std::string line;
    bool inSection = false;
    TrashItem item;
    item.trashDir = trashDir;
    item.name = info.stem().string();
    while (std::getline(in, line)) {
      if (line == "[Trash Info]") {
        inSection = true;
      } else if (!line.empty() && line.front() == '[') {
        inSection = false;
      } else if (inSection && line.starts_with("Path=")) {
        item.originalPath = decodeTrashPath(line.substr(5), trashDir);
      } else if (inSection && line.starts_with("DeletionDate=")) {
        item.deletionDate = line.substr(13);
      }
    }
    const fs::path file = trashDir / "files" / item.name;
    if (item.originalPath.empty() || !fs::exists(fs::symlink_status(file, ec))) {
      continue;
    }
    const fs::file_status status = fs::symlink_status(file, ec);
    item.isDir = fs::is_directory(status);
    const std::uintmax_t size = fs::is_regular_file(status) ? fs::file_size(file, ec) : 0;
    item.size = ec ? 0 : static_cast<std::uint64_t>(size);
    items.push_back(std::move(item));
  }
  return items;
}
