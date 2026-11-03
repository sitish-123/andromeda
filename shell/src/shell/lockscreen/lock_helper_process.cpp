#include "shell/lockscreen/lock_helper_process.h"

#include "core/log.h"
#include "core/toml.h"

#include <array>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <sstream>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <type_traits>
#include <unistd.h>
#include <variant>

namespace {

  constexpr Logger kLog("lock-helper");
  // Child fd numbers: start document, then one per slot.
  constexpr int kStartFd = 3;
  constexpr int kFirstSlotFd = 4;
  // Bounds the child's fixed fd table; a lock never has this many widgets.
  constexpr std::size_t kMaxSlots = 64;
  // Above every target number (3 + kMaxSlots), so no copy lands on a target before its dup2.
  constexpr int kScratchFd = kFirstSlotFd + static_cast<int>(kMaxSlots) + 1;

  toml::table settingsTable(const LockHelperProcess::Settings& settings) {
    toml::table table;
    for (const auto& [key, value] : settings) {
      std::visit(
          [&](const auto& concrete) {
            using T = std::decay_t<decltype(concrete)>;
            if constexpr (std::is_same_v<T, std::vector<std::string>>) {
              toml::array array;
              for (const auto& item : concrete) {
                array.push_back(item);
              }
              table.insert_or_assign(key, std::move(array));
            } else if constexpr (std::is_same_v<T, WidgetSettingStringMap>) {
              toml::table map;
              for (const auto& [k, v] : concrete) {
                map.insert_or_assign(k, v);
              }
              table.insert_or_assign(key, std::move(map));
            } else {
              table.insert_or_assign(key, concrete);
            }
          },
          value
      );
    }
    return table;
  }

  std::string startDocument(const std::vector<LockHelperProcess::Slot>& slots, const LockHelperProcess::StartData& data) {
    toml::table root;
    root.insert_or_assign("dark", data.dark);
    root.insert_or_assign("time_format", data.timeFormat);
    root.insert_or_assign("date_format", data.dateFormat);
    toml::array sources;
    for (const auto& source : data.pluginSources) {
      sources.push_back(source);
    }
    root.insert_or_assign("plugin_sources", std::move(sources));
    toml::table pluginSettings;
    for (const auto& [id, settings] : data.pluginSettings) {
      pluginSettings.insert_or_assign(id, settingsTable(settings));
    }
    root.insert_or_assign("plugin_settings", std::move(pluginSettings));
    toml::array slotArray;
    for (std::size_t i = 0; i < slots.size(); ++i) {
      const auto& slot = slots[i];
      toml::table table;
      table.insert_or_assign("fd", kFirstSlotFd + static_cast<int>(i));
      table.insert_or_assign("type", slot.type);
      table.insert_or_assign("width", static_cast<double>(slot.width));
      table.insert_or_assign("height", static_cast<double>(slot.height));
      table.insert_or_assign("scale", static_cast<double>(slot.scale));
      table.insert_or_assign("content_scale", static_cast<double>(slot.contentScale));
      table.insert_or_assign("settings", settingsTable(slot.settings));
      slotArray.push_back(std::move(table));
    }
    root.insert_or_assign("slot", std::move(slotArray));
    std::ostringstream out;
    out << root;
    return out.str();
  }

  void closeAll(std::vector<int>& fds) {
    for (int& fd : fds) {
      if (fd >= 0) {
        ::close(fd);
      }
      fd = -1;
    }
  }

} // namespace

LockHelperProcess::~LockHelperProcess() { stop(); }

std::vector<int> LockHelperProcess::start(const std::vector<Slot>& slots, const StartData& data) {
  stop();
  if (slots.empty() || slots.size() > kMaxSlots) {
    return {};
  }

  const std::string document = startDocument(slots, data);
  const int startFd = ::memfd_create("lock-helper-start", MFD_CLOEXEC);
  if (startFd < 0
      || ::write(startFd, document.data(), document.size()) != static_cast<ssize_t>(document.size())
      || ::lseek(startFd, 0, SEEK_SET) != 0) {
    if (startFd >= 0) {
      ::close(startFd);
    }
    kLog.warn("could not write the start document");
    return {};
  }

  std::vector<int> shellEnds(slots.size(), -1);
  std::vector<int> helperEnds(slots.size(), -1);
  std::array<int, 2> alive = {-1, -1};
  bool ok = ::pipe2(alive.data(), O_CLOEXEC) == 0;
  for (std::size_t i = 0; ok && i < slots.size(); ++i) {
    std::array<int, 2> pair = {-1, -1};
    ok = ::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, pair.data()) == 0;
    shellEnds[i] = pair[0];
    helperEnds[i] = pair[1];
  }
  const pid_t pid = ok ? ::fork() : -1;
  if (pid == 0) {
    // Async-signal-safe until execv. Every fd moves above the target range first: dup2 onto
    // the same number keeps FD_CLOEXEC, and a source may already sit on another target number.
    const int startCopy = ::fcntl(startFd, F_DUPFD_CLOEXEC, kScratchFd);
    const int aliveCopy = ::fcntl(alive[0], F_DUPFD_CLOEXEC, kScratchFd);
    std::array<int, kMaxSlots> copies{};
    for (std::size_t i = 0; i < helperEnds.size(); ++i) {
      copies[i] = ::fcntl(helperEnds[i], F_DUPFD_CLOEXEC, kScratchFd);
      if (copies[i] < 0) {
        ::_exit(127);
      }
    }
    if (startCopy < 0 || aliveCopy < 0 || ::dup2(aliveCopy, STDIN_FILENO) < 0 || ::dup2(startCopy, kStartFd) < 0) {
      ::_exit(127);
    }
    for (std::size_t i = 0; i < helperEnds.size(); ++i) {
      if (::dup2(copies[i], kFirstSlotFd + static_cast<int>(i)) < 0) {
        ::_exit(127);
      }
    }
    const char* argv[] = {"noctalia", "lock-helper", "--start-fd", "3", nullptr};
    ::execv("/proc/self/exe", const_cast<char* const*>(argv));
    ::_exit(127);
  }

  ::close(startFd);
  closeAll(helperEnds);
  if (alive[0] >= 0) {
    ::close(alive[0]);
  }
  if (pid < 0) {
    kLog.warn("could not start the lock helper: {}", std::strerror(errno));
    closeAll(shellEnds);
    if (alive[1] >= 0) {
      ::close(alive[1]);
    }
    return {};
  }
  m_pid = pid;
  m_alive = alive[1];
  kLog.info("started lock helper pid {} with {} slot(s)", pid, slots.size());
  return shellEnds;
}

void LockHelperProcess::stop() {
  if (m_alive >= 0) {
    ::close(m_alive);
    m_alive = -1;
  }
  if (m_pid > 0) {
    // The helper may hang in a plugin script, so EOF on stdin is not enough.
    ::kill(m_pid, SIGKILL);
    m_unreaped.push_back(m_pid);
    m_pid = -1;
  }
  // Never block: a helper in uninterruptible I/O dies only when that I/O returns.
  std::erase_if(m_unreaped, [](pid_t pid) { return ::waitpid(pid, nullptr, WNOHANG) != 0; });
}
