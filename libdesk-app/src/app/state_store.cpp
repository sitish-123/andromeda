#include "app/state_store.h"

#include "core/log.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <unistd.h>

namespace {
  constexpr Logger kLog("app-state");
  constexpr auto kSaveDelay = std::chrono::milliseconds(500);

  bool writeAll(int fd, const std::string& text) {
    std::size_t done = 0;
    while (done < text.size()) {
      const ssize_t n = ::write(fd, text.data() + done, text.size() - done);
      if (n < 0) {
        if (errno == EINTR) {
          continue;
        }
        return false;
      }
      done += static_cast<std::size_t>(n);
    }
    return true;
  }
} // namespace

StateStore::StateStore(std::filesystem::path file) : m_file(std::move(file)) {}

StateStore::~StateStore() {
  if (m_dirty) {
    flush();
  }
}

std::filesystem::path StateStore::pathFor(std::string_view appId) {
  std::filesystem::path base;
  if (const char* state = std::getenv("XDG_STATE_HOME"); state != nullptr && state[0] == '/') {
    base = state;
  } else if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
    base = std::filesystem::path(home) / ".local/state";
  } else {
    base = "/tmp";
  }
  return base / "andromeda" / std::string(appId) / "state.json";
}

void StateStore::load() {
  m_data = nlohmann::json::object();
  std::ifstream in(m_file);
  if (!in) {
    return;
  }
  try {
    nlohmann::json parsed = nlohmann::json::parse(in);
    if (parsed.is_object()) {
      m_data = std::move(parsed);
      return;
    }
  } catch (const nlohmann::json::exception& e) {
    kLog.warn("{} is not valid JSON ({}); starting with empty state", m_file.string(), e.what());
  }
  std::error_code ec;
  std::filesystem::rename(m_file, m_file.string() + ".bad", ec);
}

void StateStore::set(std::string_view pointer, nlohmann::json value) {
  const nlohmann::json::json_pointer ptr{std::string(pointer)};
  if (m_data.contains(ptr) && m_data.at(ptr) == value) {
    return;
  }
  m_data[ptr] = std::move(value);
  m_dirty = true;
  m_saveTimer.start(kSaveDelay, [this] { flush(); });
}

bool StateStore::flush() {
  m_saveTimer.stop();
  std::error_code ec;
  std::filesystem::create_directories(m_file.parent_path(), ec);
  const std::string tmp = m_file.string() + ".tmp";
  const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (fd < 0) {
    kLog.warn("cannot write {}: {}", tmp, std::strerror(errno));
    return false;
  }
  const bool ok = writeAll(fd, m_data.dump(2) + "\n") && ::fsync(fd) == 0;
  ::close(fd);
  if (!ok || ::rename(tmp.c_str(), m_file.c_str()) != 0) {
    kLog.warn("cannot save {}: {}", m_file.string(), std::strerror(errno));
    ::unlink(tmp.c_str());
    return false;
  }
  m_dirty = false;
  return true;
}
