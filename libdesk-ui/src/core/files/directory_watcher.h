#pragma once

#include "core/poll_source.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

// inotify on directories, one fd per process (a PollSource on the app loop). A rename inside one watched directory
// arrives as Renamed; a move out of or into it as Deleted or Created.
class DirectoryWatcher final : public PollSource {
public:
  enum class Event : std::uint8_t {
    Created,
    Deleted,
    Changed,  // content or attributes
    Renamed,  // name -> newName
    Gone,     // the watched directory itself was deleted or moved away
    Overflow, // the kernel dropped events: rescan
  };
  using Callback = std::function<void(Event event, const std::string& name, const std::string& newName)>;
  using WatchId = int;

  DirectoryWatcher();
  ~DirectoryWatcher() override;

  DirectoryWatcher(const DirectoryWatcher&) = delete;
  DirectoryWatcher& operator=(const DirectoryWatcher&) = delete;

  // -1 when the directory cannot be watched (no inotify, no permission, watch limit).
  WatchId watch(const std::filesystem::path& dir, Callback callback);
  void unwatch(WatchId id);

  [[nodiscard]] int pollTimeoutMs() const override;
  void dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) override;
  // Reads and delivers what is queued; for loops that poll the fd themselves.
  void processEvents();
  [[nodiscard]] int fd() const noexcept { return m_fd; }

private:
  void doAddPollFds(std::vector<pollfd>& fds) override;
  // Delivers Deleted for moves out whose pair time passed.
  void flushPendingMoves();

  struct Watch {
    std::vector<std::pair<int, Callback>> subscribers;
  };
  struct PendingMove {
    int wd;
    std::uint32_t cookie;
    std::string name;
    std::chrono::steady_clock::time_point deadline;
  };

  int m_fd = -1;
  int m_nextId = 1;
  std::unordered_map<int, Watch> m_watches;       // by inotify wd
  std::unordered_map<int, int> m_idToWd;          // by WatchId
  std::vector<PendingMove> m_pendingMoves;        // IN_MOVED_FROM waiting for a matching IN_MOVED_TO
};
