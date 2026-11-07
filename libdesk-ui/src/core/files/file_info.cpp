#include "core/files/file_info.h"

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
} // namespace

std::optional<FileInfo> statFile(int dirFd, std::string_view nameView) {
  const std::string name(nameView);
  struct stat link{};
  if (::fstatat(dirFd, name.c_str(), &link, AT_SYMLINK_NOFOLLOW) != 0) {
    return std::nullopt;
  }
  FileInfo info;
  info.name = name;
  info.hidden = !name.empty() && name.front() == '.';
  struct stat target = link;
  if (S_ISLNK(link.st_mode)) {
    info.kind = FileKind::Symlink;
    // A dangling link keeps its own stat.
    if (::fstatat(dirFd, name.c_str(), &target, 0) != 0) {
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
  info.displayName = ownedString(g_filename_display_name(name.c_str()));
  info.collateKey = ownedString(g_utf8_collate_key_for_filename(info.displayName.c_str(), -1));
  if (info.isDir) {
    info.contentType = "inode/directory";
  } else {
    gboolean uncertain = FALSE;
    gchar* type = g_content_type_guess(name.c_str(), nullptr, 0, &uncertain);
    gchar* mime = type != nullptr ? g_content_type_get_mime_type(type) : nullptr;
    g_free(type);
    info.contentType = mime != nullptr ? ownedString(mime) : "application/octet-stream";
  }
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
