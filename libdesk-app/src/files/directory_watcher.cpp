#include "files/directory_watcher.h"

#include "core/log.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <sys/inotify.h>
#include <unistd.h>

namespace {
  constexpr Logger kLog("dir-watch");
  constexpr std::uint32_t kMask = IN_CREATE | IN_DELETE | IN_MODIFY | IN_ATTRIB | IN_CLOSE_WRITE | IN_MOVED_FROM
      | IN_MOVED_TO | IN_DELETE_SELF | IN_MOVE_SELF | IN_ONLYDIR | IN_EXCL_UNLINK;
  // A MOVED_FROM whose MOVED_TO does not come within this time was a move out of the directory.
  constexpr int kMovePairMs = 20;
} // namespace

DirectoryWatcher::DirectoryWatcher() : m_fd(inotify_init1(IN_NONBLOCK | IN_CLOEXEC)) {
  if (m_fd < 0) {
    kLog.warn("inotify unavailable: {}", std::strerror(errno));
  }
}

DirectoryWatcher::~DirectoryWatcher() {
  if (m_fd >= 0) {
    ::close(m_fd);
  }
}

DirectoryWatcher::WatchId DirectoryWatcher::watch(const std::filesystem::path& dir, Callback callback) {
  if (m_fd < 0) {
    return -1;
  }
  const int wd = inotify_add_watch(m_fd, dir.c_str(), kMask);
  if (wd < 0) {
    kLog.warn("cannot watch {}: {}", dir.string(), std::strerror(errno));
    return -1;
  }
  const int id = m_nextId++;
  // inotify returns the same wd for the same directory: subscribers share it.
  m_watches[wd].subscribers.emplace_back(id, std::move(callback));
  m_idToWd[id] = wd;
  return id;
}

void DirectoryWatcher::unwatch(WatchId id) {
  const auto it = m_idToWd.find(id);
  if (it == m_idToWd.end()) {
    return;
  }
  const int wd = it->second;
  m_idToWd.erase(it);
  auto watch = m_watches.find(wd);
  if (watch == m_watches.end()) {
    return;
  }
  std::erase_if(watch->second.subscribers, [id](const auto& s) { return s.first == id; });
  if (watch->second.subscribers.empty()) {
    m_watches.erase(watch);
    inotify_rm_watch(m_fd, wd);
    std::erase_if(m_pendingMoves, [wd](const PendingMove& m) { return m.wd == wd; });
  }
}

int DirectoryWatcher::pollTimeoutMs() const {
  if (m_pendingMoves.empty()) {
    return -1;
  }
  const auto left = std::chrono::ceil<std::chrono::milliseconds>(
      m_pendingMoves.front().deadline - std::chrono::steady_clock::now()
  );
  return static_cast<int>(std::max<std::int64_t>(0, left.count()));
}

void DirectoryWatcher::doAddPollFds(std::vector<pollfd>& fds) {
  if (m_fd >= 0) {
    fds.push_back({.fd = m_fd, .events = POLLIN, .revents = 0});
  }
}

void DirectoryWatcher::dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) {
  const bool readable = startIdx < fds.size() && fds[startIdx].fd == m_fd && fds[startIdx].revents != 0;
  if (readable) {
    processEvents();
  }
  flushPendingMoves();
}

void DirectoryWatcher::processEvents() {
  if (m_fd < 0) {
    return;
  }
  // Copies: a callback may unwatch or watch while events for the same wd are still queued.
  const auto deliver = [this](int wd, Event event, const std::string& name, const std::string& newName = {}) {
    const auto it = m_watches.find(wd);
    if (it == m_watches.end()) {
      return;
    }
    const auto subscribers = it->second.subscribers;
    for (const auto& [id, callback] : subscribers) {
      if (m_idToWd.contains(id) && callback) {
        callback(event, name, newName);
      }
    }
  };

  alignas(inotify_event) std::array<char, 16384> buffer{};
  for (;;) {
    const ssize_t n = ::read(m_fd, buffer.data(), buffer.size());
    if (n <= 0) {
      if (n < 0 && errno == EINTR) {
        continue;
      }
      break;
    }
    for (ssize_t off = 0; off < n;) {
      const auto* ev = reinterpret_cast<const inotify_event*>(buffer.data() + off);
      off += static_cast<ssize_t>(sizeof(inotify_event) + ev->len);
      const std::string name = ev->len > 0 ? std::string(ev->name) : std::string{};
      if ((ev->mask & IN_Q_OVERFLOW) != 0) {
        m_pendingMoves.clear();
        std::vector<int> wds;
        for (const auto& [wd, _] : m_watches) {
          wds.push_back(wd);
        }
        for (const int wd : wds) {
          deliver(wd, Event::Overflow, {});
        }
        continue;
      }
      if ((ev->mask & (IN_DELETE_SELF | IN_MOVE_SELF)) != 0) {
        deliver(ev->wd, Event::Gone, {});
        continue;
      }
      if ((ev->mask & IN_IGNORED) != 0) {
        continue;
      }
      // A later event for a name that moved out: the move-out happened first, deliver it first.
      if (const auto out = std::ranges::find_if(m_pendingMoves, [&](const PendingMove& m) {
            return m.wd == ev->wd && m.name == name && (m.cookie != ev->cookie || (ev->mask & IN_MOVED_TO) == 0);
          });
          !name.empty() && out != m_pendingMoves.end()) {
        const PendingMove move = *out;
        m_pendingMoves.erase(out);
        deliver(move.wd, Event::Deleted, move.name);
      }
      if ((ev->mask & IN_MOVED_FROM) != 0) {
        m_pendingMoves.push_back({
            .wd = ev->wd,
            .cookie = ev->cookie,
            .name = name,
            .deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kMovePairMs),
        });
        continue;
      }
      if ((ev->mask & IN_MOVED_TO) != 0) {
        const auto from = std::ranges::find_if(m_pendingMoves, [&](const PendingMove& m) { return m.cookie == ev->cookie; });
        if (from == m_pendingMoves.end()) {
          deliver(ev->wd, Event::Created, name);
          continue;
        }
        const PendingMove move = *from;
        m_pendingMoves.erase(from);
        if (move.wd == ev->wd) {
          deliver(ev->wd, Event::Renamed, move.name, name);
        } else {
          deliver(move.wd, Event::Deleted, move.name);
          deliver(ev->wd, Event::Created, name);
        }
        continue;
      }
      if ((ev->mask & IN_CREATE) != 0) {
        deliver(ev->wd, Event::Created, name);
      } else if ((ev->mask & IN_DELETE) != 0) {
        deliver(ev->wd, Event::Deleted, name);
      } else if ((ev->mask & (IN_MODIFY | IN_ATTRIB | IN_CLOSE_WRITE)) != 0) {
        deliver(ev->wd, Event::Changed, name);
      }
    }
  }
  flushPendingMoves();
}

void DirectoryWatcher::flushPendingMoves() {
  const auto now = std::chrono::steady_clock::now();
  std::vector<PendingMove> due;
  std::erase_if(m_pendingMoves, [&](const PendingMove& m) {
    if (now >= m.deadline) {
      due.push_back(m);
      return true;
    }
    return false;
  });
  for (const PendingMove& move : due) {
    const auto it = m_watches.find(move.wd);
    if (it == m_watches.end()) {
      continue;
    }
    const auto subscribers = it->second.subscribers;
    for (const auto& [id, callback] : subscribers) {
      if (m_idToWd.contains(id) && callback) {
        callback(Event::Deleted, move.name, {});
      }
    }
  }
}
