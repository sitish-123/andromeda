#include "state_file.h"

#include "core/log.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <unistd.h>

namespace {
  constexpr Logger kLog("state-file");
} // namespace

bool writeStateFile(const std::filesystem::path& path, std::string_view content) {
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  const std::string tmp = path.string() + ".tmp";
  const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
  bool ok = fd >= 0;
  for (std::size_t done = 0; ok && done < content.size();) {
    const ssize_t n = ::write(fd, content.data() + done, content.size() - done);
    if (n < 0 && errno == EINTR) {
      continue;
    }
    ok = n > 0;
    done += ok ? static_cast<std::size_t>(n) : 0;
  }
  ok = ok && ::fsync(fd) == 0;
  if (fd >= 0) {
    ok = ::close(fd) == 0 && ok;
  }
  ok = ok && ::rename(tmp.c_str(), path.c_str()) == 0;
  if (!ok) {
    kLog.warn("save {}: {}", path.string(), std::strerror(errno));
    ::unlink(tmp.c_str());
  }
  return ok;
}
