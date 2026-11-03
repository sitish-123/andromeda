#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace andromeda::index {

  // Bytes of text kept per file.
  inline constexpr std::size_t kMaxText = 256 * 1024;

  enum class DocumentKind : std::uint8_t { None, Text, Html, Pdf, Zip };

  // Lowercase extension of a file name, or empty.
  [[nodiscard]] std::string extensionOf(std::string_view name);
  [[nodiscard]] DocumentKind documentKind(std::string_view name);
  // True when the file name has an extension extractText() reads: plain text, Markdown and other text documents,
  // HTML, PDF, OpenDocument, Office Open XML and EPUB. Source and config files are not read.
  [[nodiscard]] bool extractable(std::string_view name);

  // The text of a file (andromeda-index-extract only: it loads poppler and libzip), at most kMaxText bytes, or nullopt when it cannot be read (binary content, a broken
  // archive or PDF).
  [[nodiscard]] std::optional<std::string> extractText(const std::string& path);

  // Markup without its tags: each tag becomes a space; script and style bodies are dropped; common entities decoded.
  [[nodiscard]] std::string stripTags(std::string_view markup, std::size_t maxBytes);

} // namespace andromeda::index
