#include "config/legacy_home_move.h"

#include "core/log.h"
#include "util/file_utils.h"

#include <cstdlib>
#include <fcntl.h>
#include <string>
#include <sys/file.h>
#include <system_error>
#include <unistd.h>

namespace noctalia::config {

  namespace {

    constexpr Logger kLog("config");
    constexpr const char* kLegacyName = "noctalia";

    std::filesystem::path cacheDir() {
      if (const char* xdg = std::getenv("XDG_CACHE_HOME"); xdg != nullptr && xdg[0] != '\0') {
        return std::filesystem::path(xdg) / "andromeda" / "shell";
      }
      if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
        return std::filesystem::path(home) / ".cache" / "andromeda" / "shell";
      }
      return {};
    }

    // Serialises the move between processes that start together (shell, `lock`, `config`).
    class MoveLock {
    public:
      explicit MoveLock(const std::filesystem::path& file)
          : m_fd(::open(file.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600)) {
        if (m_fd >= 0 && ::flock(m_fd, LOCK_EX) != 0) {
          ::close(m_fd);
          m_fd = -1;
        }
      }
      MoveLock(const MoveLock&) = delete;
      MoveLock& operator=(const MoveLock&) = delete;
      ~MoveLock() {
        if (m_fd >= 0) {
          ::close(m_fd);
        }
      }
      [[nodiscard]] bool held() const { return m_fd >= 0; }

    private:
      int m_fd;
    };

    bool isLinkTo(const std::filesystem::path& legacy, const std::filesystem::path& target) {
      std::error_code ec;
      return std::filesystem::is_symlink(legacy, ec) && std::filesystem::equivalent(legacy, target, ec);
    }

    bool copyAcross(const std::filesystem::path& legacy, const std::filesystem::path& target, std::error_code& ec) {
      std::filesystem::path partial = target;
      partial += ".partial";
      std::filesystem::remove_all(partial, ec);
      std::filesystem::copy(
          legacy, partial, std::filesystem::copy_options::recursive | std::filesystem::copy_options::copy_symlinks, ec
      );
      if (!ec) {
        std::filesystem::rename(partial, target, ec);
      }
      if (ec) {
        std::error_code ignored;
        std::filesystem::remove_all(partial, ignored);
        return false;
      }
      std::filesystem::remove_all(legacy, ec);
      return !ec;
    }

    // Recreates the link `legacy` at `target`; a relative link value becomes absolute so it still resolves.
    bool moveLink(const std::filesystem::path& legacy, const std::filesystem::path& target, std::error_code& ec) {
      std::filesystem::path value = std::filesystem::read_symlink(legacy, ec);
      if (ec) {
        return false;
      }
      if (value.is_relative()) {
        value = (legacy.parent_path() / value).lexically_normal();
      }
      std::filesystem::create_directory_symlink(value, target, ec);
      if (!ec) {
        std::filesystem::remove(legacy, ec);
      }
      return !ec;
    }

    void linkLegacy(const std::filesystem::path& legacy, const std::filesystem::path& target) {
      std::error_code ec;
      std::filesystem::path link = target.lexically_relative(legacy.parent_path());
      std::filesystem::create_directory_symlink(link.empty() ? target : link, legacy, ec);
      if (ec) {
        kLog.warn("moved {} to {}, but cannot link the old path: {}", legacy.string(), target.string(), ec.message());
      }
    }

    LegacyHomeMove moveLocked(
        const std::filesystem::path& legacy, const std::filesystem::path& target, LegacyContent content
    ) {
      std::error_code ec;
      const auto status = std::filesystem::symlink_status(legacy, ec);
      if (ec || !std::filesystem::exists(status) || isLinkTo(legacy, target)) {
        return LegacyHomeMove::Nothing;
      }
      const bool legacyIsLink = std::filesystem::is_symlink(status);
      if (legacyIsLink && !std::filesystem::is_directory(legacy, ec)) {
        return LegacyHomeMove::Nothing;
      }
      if (std::filesystem::exists(std::filesystem::symlink_status(target, ec))) {
        if (content != LegacyContent::Disposable || legacyIsLink) {
          return LegacyHomeMove::KeptTarget;
        }
        std::filesystem::remove_all(legacy, ec);
        if (ec) {
          return LegacyHomeMove::Failed;
        }
        linkLegacy(legacy, target);
        return LegacyHomeMove::DroppedLegacy;
      }
      if (legacyIsLink) {
        if (!moveLink(legacy, target, ec)) {
          return LegacyHomeMove::Failed;
        }
      } else {
        std::filesystem::rename(legacy, target, ec);
        if (ec == std::errc::cross_device_link && !copyAcross(legacy, target, ec)) {
          return LegacyHomeMove::Failed;
        }
        if (ec) {
          return LegacyHomeMove::Failed;
        }
      }
      linkLegacy(legacy, target);
      return LegacyHomeMove::Moved;
    }

  } // namespace

  LegacyHomeMove
  moveLegacyHome(const std::filesystem::path& legacy, const std::filesystem::path& target, LegacyContent content) {
    std::error_code ec;
    if (!std::filesystem::exists(std::filesystem::symlink_status(legacy, ec)) || isLinkTo(legacy, target)) {
      return LegacyHomeMove::Nothing;
    }
    std::filesystem::create_directories(target.parent_path(), ec);
    if (ec) {
      return LegacyHomeMove::Failed;
    }
    const MoveLock lock(target.parent_path() / ".shell-move.lock");
    if (!lock.held()) {
      return LegacyHomeMove::Failed;
    }
    return moveLocked(legacy, target, content);
  }

  void moveLegacyHomes() {
    const std::filesystem::path cache = cacheDir();
    for (const std::filesystem::path& target :
         {std::filesystem::path(FileUtils::configDir()), std::filesystem::path(FileUtils::stateDir()),
          std::filesystem::path(FileUtils::dataDir()), cache}) {
      if (target.empty()) {
        continue;
      }
      const std::filesystem::path legacy = target.parent_path().parent_path() / kLegacyName;
      const LegacyContent content = target == cache ? LegacyContent::Disposable : LegacyContent::Keep;
      switch (moveLegacyHome(legacy, target, content)) {
      case LegacyHomeMove::Moved:
        kLog.info("moved {} to {}", legacy.string(), target.string());
        break;
      case LegacyHomeMove::DroppedLegacy:
        kLog.info("{} and {} both exist; removed the old cache and linked it", legacy.string(), target.string());
        break;
      case LegacyHomeMove::KeptTarget:
        kLog.info("{} and {} both exist; using {}", legacy.string(), target.string(), target.string());
        break;
      case LegacyHomeMove::Failed:
        kLog.warn("cannot move {} to {}", legacy.string(), target.string());
        break;
      case LegacyHomeMove::Nothing:
        break;
      }
    }
  }

} // namespace noctalia::config
