#include "crawler.h"

#include "skip_rules.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <deque>
#include <string>
#include <unordered_set>

namespace andromeda::index {

  namespace {
    constexpr int kBatchRows = 2000;

    struct Entry {
      std::string name;
      unsigned char type;
    };
  } // namespace

  CrawlStats crawl(IndexDb& db, const std::filesystem::path& root, const std::vector<std::filesystem::path>& first) {
    CrawlStats stats;
    std::deque<std::string> queue;
    std::unordered_set<std::string> queued;
    // Roots are indexed even when they hold a marker: a home folder that is itself a git checkout (dotfiles) must
    // not hide everything below it.
    std::unordered_set<std::string> roots{root.string()};
    for (const auto& folder : first) {
      roots.insert(folder.string());
    }
    for (const auto& folder : first) {
      if (queued.insert(folder.string()).second) {
        queue.push_back(folder.string());
      }
    }
    if (queued.insert(root.string()).second) {
      queue.push_back(root.string());
    }

    int pending = 0;
    db.begin();
    while (!queue.empty()) {
      const std::string dir = std::move(queue.front());
      queue.pop_front();
      const int fd = open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
      if (fd < 0) {
        continue;
      }
      DIR* stream = fdopendir(fd);
      if (stream == nullptr) {
        close(fd);
        continue;
      }
      std::vector<Entry> entries;
      bool skip = false;
      while (const dirent* e = readdir(stream)) {
        const std::string_view name = e->d_name;
        if (name == "." || name == "..") {
          continue;
        }
        if (skipMarkerName(name) && !roots.contains(dir)) {
          skip = true;
          break;
        }
        if (!hiddenName(name)) {
          entries.push_back({std::string(name), e->d_type});
        }
      }
      if (skip) {
        ++stats.skippedFolders;
        closedir(stream);
        continue;
      }
      ++stats.folders;
      for (const Entry& entry : entries) {
        struct stat st {};
        if (fstatat(fd, entry.name.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0 || S_ISLNK(st.st_mode)) {
          continue;
        }
        const bool isDir = S_ISDIR(st.st_mode);
        if (!isDir && !S_ISREG(st.st_mode)) {
          continue;
        }
        std::string path = dir == "/" ? "/" + entry.name : dir + "/" + entry.name;
        if (isDir && skipFolderName(entry.name)) {
          ++stats.skippedFolders;
          continue;
        }
        db.upsert({.path = path, .name = entry.name, .mtime = st.st_mtim.tv_sec, .size = st.st_size, .isDir = isDir});
        if (isDir) {
          if (queued.insert(path).second) {
            queue.push_back(std::move(path));
          }
        } else {
          ++stats.files;
        }
        if (++pending >= kBatchRows) {
          db.commit();
          db.begin();
          pending = 0;
        }
      }
      closedir(stream);
    }
    db.commit();
    return stats;
  }

} // namespace andromeda::index
