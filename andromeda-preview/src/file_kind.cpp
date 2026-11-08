#include "file_kind.h"

#include <gio/gio.h>
#include <string>

namespace preview {

  FileKind kindForContentType(std::string_view contentType) {
    const std::string type(contentType);
    const char* t = type.c_str();
    if (type == "inode/directory") {
      return FileKind::Folder;
    }
    if (g_content_type_is_a(t, "application/pdf")) {
      return FileKind::Pdf;
    }
    if (type.starts_with("image/")) {
      return FileKind::Image;
    }
    if (type.starts_with("video/")) {
      return FileKind::Video;
    }
    if (type.starts_with("audio/")) {
      return FileKind::Audio;
    }
    if (type.starts_with("font/") || type.starts_with("application/x-font-") || type == "application/vnd.ms-opentype") {
      return FileKind::Font;
    }
    if (g_content_type_is_a(t, "text/plain")) {
      return FileKind::Text;
    }
    // What FileOps1 ListArchive reads. Exact types: office files are subclasses of application/zip.
    for (const char* archive : {"application/zip", "application/x-7z-compressed", "application/x-tar",
                                "application/x-compressed-tar", "application/x-xz-compressed-tar",
                                "application/x-zstd-compressed-tar", "application/x-bzip2-compressed-tar",
                                "application/x-bzip-compressed-tar"}) {
      if (type == archive) {
        return FileKind::Archive;
      }
    }
    return FileKind::Other;
  }

} // namespace preview
