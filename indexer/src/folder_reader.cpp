#include "folder_reader.h"

#include "skip_rules.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace andromeda::index {

  namespace {
    std::int64_t nanos(const struct timespec& t) { return t.tv_sec * 1'000'000'000LL + t.tv_nsec; }
  } // namespace

  FolderRead readFolder(const std::string& path, bool isRoot, dev_t device, std::vector<DiskEntry>& out) {
    out.clear();
    const int fd = open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
      return FolderRead::Missing;
    }
    DIR* stream = fdopendir(fd);
    if (stream == nullptr) {
      close(fd);
      return FolderRead::Missing;
    }
    std::vector<std::string> names;
    while (const dirent* e = readdir(stream)) {
      const std::string_view name = e->d_name;
      if (name == "." || name == "..") {
        continue;
      }
      if (skipMarkerName(name) && !isRoot) {
        closedir(stream);
        return FolderRead::Skipped;
      }
      if (!hiddenName(name)) {
        names.emplace_back(name);
      }
    }
    for (std::string& name : names) {
      struct stat st {};
      if (fstatat(fd, name.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0) {
        continue;
      }
      const bool isDir = S_ISDIR(st.st_mode);
      // Another filesystem below the root is a mount (phone, network share, removable drive): never crossed.
      if ((!isDir && !S_ISREG(st.st_mode)) || (isDir && (skipFolderName(name) || st.st_dev != device))) {
        continue;
      }
      out.push_back({std::move(name), nanos(st.st_mtim), isDir ? 0 : static_cast<std::int64_t>(st.st_size), isDir});
    }
    closedir(stream);
    return FolderRead::Ok;
  }

  std::int64_t mtimeOf(const std::string& path) {
    struct stat st {};
    return stat(path.c_str(), &st) == 0 ? nanos(st.st_mtim) : -1;
  }

} // namespace andromeda::index
