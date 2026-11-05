#pragma once

#include <cstdint>
#include <string_view>

namespace preview {

  enum class FileKind : std::uint8_t { Image, Pdf, Text, Video, Audio, Folder, Other };

  // Picks the renderer for a shared-mime-info content type (docs/search.md §5 renderer table).
  [[nodiscard]] FileKind kindForContentType(std::string_view contentType);

} // namespace preview
