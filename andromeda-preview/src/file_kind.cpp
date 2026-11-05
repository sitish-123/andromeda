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
    if (g_content_type_is_a(t, "text/plain")) {
      return FileKind::Text;
    }
    return FileKind::Other;
  }

} // namespace preview
