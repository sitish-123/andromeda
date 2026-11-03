#include "extract.h"

#include <glib.h>
#include <poppler.h>
#include <zip.h>

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <array>
#include <cctype>
#include <fstream>
#include <functional>
#include <memory>

namespace andromeda::index {

  namespace {
    // Markup read from one archive entry; a larger entry (or a zip bomb) is cut here.
    constexpr std::size_t kMaxMarkup = 4 * 1024 * 1024;

    std::optional<std::string> pdfText(const std::string& path) {
      gchar* uri = g_filename_to_uri(path.c_str(), nullptr, nullptr);
      if (uri == nullptr) {
        return std::nullopt;
      }
      GError* error = nullptr;
      PopplerDocument* doc = poppler_document_new_from_file(uri, nullptr, &error);
      g_free(uri);
      if (doc == nullptr) {
        g_clear_error(&error);
        return std::nullopt;
      }
      std::string text;
      const int pages = poppler_document_get_n_pages(doc);
      for (int i = 0; i < pages && text.size() < kMaxText; ++i) {
        PopplerPage* page = poppler_document_get_page(doc, i);
        if (page == nullptr) {
          continue;
        }
        if (gchar* pageText = poppler_page_get_text(page); pageText != nullptr) {
          text += pageText;
          text += '\n';
          g_free(pageText);
        }
        g_object_unref(page);
      }
      g_object_unref(doc);
      text.resize(std::min(text.size(), kMaxText));
      return text;
    }

    // The entries of an office or EPUB archive that hold its text.
    std::function<bool(std::string_view)> textEntries(const std::string& ext) {
      if (ext == "docx") {
        return [](std::string_view e) { return e == "word/document.xml"; };
      }
      if (ext == "xlsx") {
        return [](std::string_view e) { return e == "xl/sharedStrings.xml"; };
      }
      if (ext == "pptx") {
        return [](std::string_view e) { return e.starts_with("ppt/slides/slide") && e.ends_with(".xml"); };
      }
      if (ext == "epub") {
        return [](std::string_view e) { return e.ends_with(".xhtml") || e.ends_with(".html") || e.ends_with(".htm"); };
      }
      return [](std::string_view e) { return e == "content.xml"; }; // OpenDocument
    }

    std::optional<std::string> zipText(const std::string& path) {
      int error = 0;
      zip_t* archive = zip_open(path.c_str(), ZIP_RDONLY, &error);
      if (archive == nullptr) {
        return std::nullopt;
      }
      const auto wanted = textEntries(extensionOf(path));
      std::string text;
      const zip_int64_t count = zip_get_num_entries(archive, 0);
      std::string markup;
      std::array<char, 16384> buffer{};
      for (zip_int64_t i = 0; i < count && text.size() < kMaxText; ++i) {
        const char* name = zip_get_name(archive, static_cast<zip_uint64_t>(i), 0);
        if (name == nullptr || !wanted(name)) {
          continue;
        }
        zip_file_t* entry = zip_fopen_index(archive, static_cast<zip_uint64_t>(i), 0);
        if (entry == nullptr) {
          continue;
        }
        markup.clear();
        zip_int64_t n = 0;
        while (markup.size() < kMaxMarkup && (n = zip_fread(entry, buffer.data(), buffer.size())) > 0) {
          markup.append(buffer.data(), static_cast<std::size_t>(n));
        }
        zip_fclose(entry);
        text += stripTags(markup, kMaxText - text.size());
        text += '\n';
      }
      zip_close(archive);
      return text;
    }

  } // namespace

  std::optional<std::string> extractText(const std::string& path) {
    switch (documentKind(path)) {
    case DocumentKind::Text:
    case DocumentKind::Html:
      return extractPlain(path);
    case DocumentKind::Pdf:
      return pdfText(path);
    case DocumentKind::Zip:
      return zipText(path);
    case DocumentKind::None:
      break;
    }
    return std::nullopt;
  }

} // namespace andromeda::index
