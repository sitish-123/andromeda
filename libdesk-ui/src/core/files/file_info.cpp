#include "core/files/file_info.h"

#include <dirent.h>
#include <fcntl.h>
#include <gio/gio.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {
  std::string ownedString(gchar* text) {
    std::string out = text != nullptr ? text : "";
    g_free(text);
    return out;
  }

  bool filenamesAreUtf8() {
    static const bool utf8 = g_get_filename_charsets(nullptr) != FALSE;
    return utf8;
  }

  // The fields that come from the name and kind alone.
  void describeName(FileInfo& info) {
    info.hidden = !info.name.empty() && info.name.front() == '.';
    if (filenamesAreUtf8() && g_utf8_validate(info.name.data(), static_cast<gssize>(info.name.size()), nullptr)) {
      info.displayName = info.name;
    } else {
      info.displayName = ownedString(g_filename_display_name(info.name.c_str()));
    }
    info.collateKey = ownedString(g_utf8_collate_key_for_filename(info.displayName.c_str(), -1));
    if (info.isDir) {
      info.contentType = "inode/directory";
    } else {
      gboolean uncertain = FALSE;
      gchar* type = g_content_type_guess(info.name.c_str(), nullptr, 0, &uncertain);
      gchar* mime = type != nullptr ? g_content_type_get_mime_type(type) : nullptr;
      g_free(type);
      info.contentType = mime != nullptr ? ownedString(mime) : "application/octet-stream";
    }
  }
} // namespace

bool fillStat(int dirFd, FileInfo& info) {
  // statx for the birth time (Date Added); the same cost as fstatat.
  struct statx sx{};
  if (::statx(dirFd, info.name.c_str(), AT_SYMLINK_NOFOLLOW, STATX_BASIC_STATS | STATX_BTIME, &sx) != 0) {
    return false;
  }
  struct stat link{};
  link.st_mode = sx.stx_mode;
  link.st_size = static_cast<off_t>(sx.stx_size);
  link.st_ino = sx.stx_ino;
  link.st_mtim = {.tv_sec = sx.stx_mtime.tv_sec, .tv_nsec = sx.stx_mtime.tv_nsec};
  const auto& added = (sx.stx_mask & STATX_BTIME) != 0 ? sx.stx_btime : sx.stx_ctime;
  info.addedNs = static_cast<std::int64_t>(added.tv_sec) * 1'000'000'000 + added.tv_nsec;
  struct stat target = link;
  info.kind = FileKind::File;
  if (S_ISLNK(link.st_mode)) {
    info.kind = FileKind::Symlink;
    // A dangling link keeps its own stat.
    if (::fstatat(dirFd, info.name.c_str(), &target, 0) != 0) {
      target = link;
    }
  } else if (S_ISDIR(link.st_mode)) {
    info.kind = FileKind::Directory;
  } else if (!S_ISREG(link.st_mode)) {
    info.kind = FileKind::Other;
  }
  info.isDir = S_ISDIR(target.st_mode);
  info.size = info.isDir ? 0 : static_cast<std::uint64_t>(target.st_size);
  info.mtimeNs = static_cast<std::int64_t>(target.st_mtim.tv_sec) * 1'000'000'000 + target.st_mtim.tv_nsec;
  info.mode = target.st_mode;
  info.inode = link.st_ino;
  info.pendingStat = false;
  return true;
}

std::optional<FileInfo> statFile(int dirFd, std::string_view name) {
  FileInfo info;
  info.name = name;
  if (!fillStat(dirFd, info)) {
    return std::nullopt;
  }
  describeName(info);
  return info;
}

std::optional<FileInfo> fileFromDirent(int dirFd, std::string_view name, unsigned char dType) {
  if (dType != DT_REG && dType != DT_DIR) {
    return statFile(dirFd, name);
  }
  FileInfo info;
  info.name = name;
  info.isDir = dType == DT_DIR;
  info.kind = info.isDir ? FileKind::Directory : FileKind::File;
  info.pendingStat = true;
  describeName(info);
  return info;
}

std::optional<FileInfo> statFile(const std::filesystem::path& path) {
  const int dirFd = ::open(path.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (dirFd < 0) {
    return std::nullopt;
  }
  auto info = statFile(dirFd, path.filename().string());
  ::close(dirFd);
  return info;
}
