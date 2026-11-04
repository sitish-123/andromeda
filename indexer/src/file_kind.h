#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace andromeda::index {

  // What a query's kind:<word> filter selects. Files are sorted by extension; Other matches no filter.
  enum class FileKind : std::uint8_t { Any, Folder, Pdf, Image, Video, Audio, Doc, Sheet, Slides, Other };

  [[nodiscard]] FileKind fileKind(std::string_view name, bool isDir);
  // The kind for a filter word (folder pdf image video audio doc sheet slides), or nullopt.
  [[nodiscard]] std::optional<FileKind> parseKind(std::string_view word);

} // namespace andromeda::index
