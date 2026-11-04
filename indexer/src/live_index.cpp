#include "live_index.h"

#include "folder_reader.h"

#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <ctime>
#include <unordered_map>
#include <vector>

namespace andromeda::index {

  namespace {
    constexpr std::uint32_t kWatchMask = IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_CLOSE_WRITE
        | IN_DELETE_SELF | IN_MOVE_SELF | IN_ONLYDIR | IN_DONT_FOLLOW | IN_EXCL_UNLINK;
    constexpr std::int64_t kNanosPerDay = 86'400LL * 1'000'000'000LL;

    std::string join(const std::string& folder, const std::string& name) {
      return folder.ends_with('/') ? folder + name : folder + "/" + name;
    }
  } // namespace

  LiveIndex::LiveIndex(IndexDb& db, std::filesystem::path root) : m_db(db), m_root(root.string()) {
    struct stat st {};
    if (stat(m_root.c_str(), &st) == 0) {
      m_device = st.st_dev;
    }
  }

  LiveIndex::~LiveIndex() {
    if (m_fd >= 0) {
      close(m_fd);
    }
  }

  int LiveIndex::start() {
    m_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (m_fd < 0) {
      return -1;
    }
    const std::int64_t recent = static_cast<std::int64_t>(std::time(nullptr)) * 1'000'000'000LL - kRecentDays * kNanosPerDay;
    for (const Folder& folder : m_db.folders()) {
      const bool top = folder.path == m_root || folder.path == join(m_root, "Desktop")
          || folder.path == join(m_root, "Documents") || folder.path == join(m_root, "Downloads");
      if (top || folder.mtime >= recent) {
        watch(folder.id, folder.path);
      }
    }
    return m_fd;
  }

  void LiveIndex::watch(std::int64_t id, const std::string& path) {
    const int wd = inotify_add_watch(m_fd, path.c_str(), kWatchMask);
    if (wd >= 0) {
      m_byWatch[wd] = {id, path};
      m_watchOf[id] = wd;
    }
  }

  void LiveIndex::unwatch(std::int64_t id) {
    if (const auto it = m_watchOf.find(id); it != m_watchOf.end()) {
      inotify_rm_watch(m_fd, it->second);
      m_byWatch.erase(it->second);
      m_watchOf.erase(it);
    }
  }

  void LiveIndex::readEvents() {
    alignas(inotify_event) std::array<char, 64 * 1024> buffer{};
    while (true) {
      const ssize_t n = read(m_fd, buffer.data(), buffer.size());
      if (n <= 0) {
        return;
      }
      for (ssize_t offset = 0; offset < n;) {
        const auto* event = reinterpret_cast<const inotify_event*>(buffer.data() + offset);
        offset += static_cast<ssize_t>(sizeof(inotify_event) + event->len);
        if ((event->mask & IN_Q_OVERFLOW) != 0) {
          sweep();
          continue;
        }
        const auto it = m_byWatch.find(event->wd);
        if (it == m_byWatch.end()) {
          continue;
        }
        if ((event->mask & IN_IGNORED) != 0) {
          m_watchOf.erase(it->second.id);
          m_byWatch.erase(it);
          continue;
        }
        // A folder that moved or vanished is settled by rescanning its parent; its own events name it only.
        if ((event->mask & (IN_DELETE_SELF | IN_MOVE_SELF)) == 0) {
          m_dirty[it->second.id] = it->second.path;
        }
      }
    }
  }

  void LiveIndex::flush() {
    if (m_dirty.empty()) {
      return;
    }
    auto dirty = std::move(m_dirty);
    m_dirty.clear();
    m_db.begin();
    for (const auto& [id, path] : dirty) {
      rescan(id, path);
    }
    m_db.commit();
  }

  void LiveIndex::sweep() {
    const std::vector<Folder> all = m_db.folders();
    for (const Folder& folder : all) {
      if (mtimeOf(folder.path) != folder.mtime) {
        m_dirty[folder.id] = folder.path;
      }
    }
    flush();
  }

  void LiveIndex::rescan(std::int64_t id, const std::string& path) {
    std::vector<DiskEntry> entries;
    const FolderRead read = readFolder(path, path == m_root, m_device, entries);
    if (read == FolderRead::Missing) {
      // The parent's rescan removes the row; only the watch goes here.
      unwatch(id);
      return;
    }
    std::unordered_map<std::string, Child> known;
    for (Child& child : m_db.children(id)) {
      known.emplace(child.name, std::move(child));
    }
    // A marker that appeared (git init) empties the folder in the index; `entries` is empty then.
    for (const DiskEntry& entry : entries) {
      const auto it = known.find(entry.name);
      if (it != known.end() && it->second.isDir == entry.isDir) {
        if (it->second.mtime != entry.mtime || it->second.size != entry.size) {
          m_db.update(it->second.id, it->second.isDir ? it->second.mtime : entry.mtime, entry.size);
        }
        known.erase(it);
        continue;
      }
      if (it != known.end()) {
        unwatch(it->second.id);
        m_db.removeTree(it->second.id);
        known.erase(it);
      }
      // A new folder: its stored time stays 0 until its own rescan below, so a sweep never skips it.
      const std::int64_t childId = m_db.add(id, entry.name, entry.isDir ? 0 : entry.mtime, entry.size, entry.isDir);
      if (entry.isDir) {
        const std::string childPath = join(path, entry.name);
        watch(childId, childPath);
        rescan(childId, childPath);
      }
    }
    for (const auto& [name, child] : known) {
      if (child.isDir) {
        unwatch(child.id);
      }
      m_db.removeTree(child.id);
    }
    // The folder's own time is recorded after its rows match the disk, so sweep() sees it as settled.
    m_db.update(id, mtimeOf(path), 0);
  }

} // namespace andromeda::index
