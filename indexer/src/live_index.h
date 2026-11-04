#pragma once

#include "index_db.h"

#include <sys/types.h>

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <unordered_map>

namespace andromeda::index {

  // Keeps an index current. inotify watches the folders changed in the last kRecentDays plus the root and its
  // Desktop, Documents and Downloads; sweep() finds changes in every other folder by comparing folder times.
  // Events only mark a folder dirty; flush() rescans dirty folders against their rows.
  class LiveIndex {
  public:
    static constexpr int kRecentDays = 30;

    LiveIndex(IndexDb& db, std::filesystem::path root);
    ~LiveIndex();
    LiveIndex(const LiveIndex&) = delete;
    LiveIndex& operator=(const LiveIndex&) = delete;

    // Sets the watches; returns the inotify fd to poll, or -1.
    int start();
    // Reads pending inotify events (non-blocking) and marks their folders dirty.
    void readEvents();
    // Rescans the dirty folders.
    void flush();
    // Rescans every indexed folder whose time on disk differs from the index (the fallback for unwatched folders
    // and for a full event queue).
    void sweep();
    [[nodiscard]] bool dirty() const { return !m_dirty.empty(); }
    [[nodiscard]] std::size_t watchCount() const { return m_byWatch.size(); }

  private:
    struct Watched {
      std::int64_t id;
      std::string path;
    };

    void watch(std::int64_t id, const std::string& path);
    void unwatch(std::int64_t id);
    void rescan(std::int64_t id, const std::string& path);

    IndexDb& m_db;
    std::string m_root;
    dev_t m_device = 0;
    int m_fd = -1;
    std::unordered_map<int, Watched> m_byWatch;
    std::unordered_map<std::int64_t, int> m_watchOf;
    std::map<std::int64_t, std::string> m_dirty; // folder id -> path
  };

} // namespace andromeda::index
