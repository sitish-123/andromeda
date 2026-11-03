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

    std::optional<std::string> readHead(const std::string& path, std::size_t maxBytes) {
      std::ifstream in(path, std::ios::binary);
      if (!in) {
        return std::nullopt;
      }
      std::string data(maxBytes, '\0');
      in.read(data.data(), static_cast<std::streamsize>(maxBytes));
      data.resize(static_cast<std::size_t>(in.gcount()));
      return data;
    }

    // The encoding of text that holds NUL bytes: UTF-16 or UTF-32 by byte order mark, else by where the NULs of
    // ASCII characters fall in the first 4 KB; nullptr for binary data.
    const char* wideEncoding(std::string_view data) {
      const auto starts = [&](std::string_view bom) { return data.starts_with(bom); };
      if (starts(std::string_view("\xFF\xFE\0\0", 4))) {
        return "UTF-32LE";
      }
      if (starts(std::string_view("\0\0\xFE\xFF", 4))) {
        return "UTF-32BE";
      }
      if (starts("\xFF\xFE")) {
        return "UTF-16LE";
      }
      if (starts("\xFE\xFF")) {
        return "UTF-16BE";
      }
      const std::size_t n = std::min<std::size_t>(data.size(), 4096) & ~std::size_t{3};
      if (n < 16) {
        return nullptr;
      }
      std::array<std::size_t, 4> zeros{};
      for (std::size_t i = 0; i < n; ++i) {
        zeros[i % 4] += data[i] == '\0' ? 1 : 0;
      }
      const auto mostly = [&](std::size_t lane) { return zeros[lane] * 10 >= (n / 4) * 9; };
      const auto rarely = [&](std::size_t lane) { return zeros[lane] * 10 <= (n / 4); };
      if (rarely(0) && mostly(1) && mostly(2) && mostly(3)) {
        return "UTF-32LE";
      }
      if (mostly(0) && mostly(1) && mostly(2) && rarely(3)) {
        return "UTF-32BE";
      }
      if (rarely(0) && mostly(1) && rarely(2) && mostly(3)) {
        return "UTF-16LE";
      }
      if (mostly(0) && rarely(1) && mostly(2) && rarely(3)) {
        return "UTF-16BE";
      }
      return nullptr;
    }

    std::optional<std::string> plainText(const std::string& path) {
      auto data = readHead(path, kMaxText);
      if (!data || data->find('\0') >= 4096) {
        return data;
      }
      // NULs near the start: UTF-16 or UTF-32 text, or a binary file under a text extension.
      const char* encoding = wideEncoding(*data);
      if (encoding == nullptr) {
        return std::nullopt;
      }
      gsize written = 0;
      gchar* utf8 = g_convert(data->data(), static_cast<gssize>(data->size() & ~std::size_t{3}), "UTF-8", encoding,
                              nullptr, &written, nullptr);
      if (utf8 == nullptr) {
        return std::nullopt;
      }
      std::string text(utf8, written);
      g_free(utf8);
      return text;
    }

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

    bool tagIs(std::string_view markup, std::size_t at, std::string_view name) {
      if (at + name.size() > markup.size()) {
        return false;
      }
      for (std::size_t i = 0; i < name.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(markup[at + i])) != name[i]) {
          return false;
        }
      }
      return true;
    }
  } // namespace

  std::string stripTags(std::string_view markup, std::size_t maxBytes) {
    constexpr std::array<std::pair<std::string_view, char>, 6> kEntities = {
        {{"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''}, {"&nbsp;", ' '}}};
    std::string out;
    out.reserve(std::min(markup.size(), maxBytes));
    std::size_t i = 0;
    while (i < markup.size() && out.size() < maxBytes) {
      const char c = markup[i];
      if (c == '<') {
        // Script and style bodies are code, not text.
        for (const std::string_view skipped : {std::string_view("script"), std::string_view("style")}) {
          if (tagIs(markup, i + 1, skipped)) {
            const std::size_t close = markup.find(std::string("</") + std::string(skipped), i);
            i = close == std::string_view::npos ? markup.size() : close;
            break;
          }
        }
        const std::size_t end = markup.find('>', i);
        i = end == std::string_view::npos ? markup.size() : end + 1;
        if (!out.empty() && out.back() != ' ') {
          out += ' ';
        }
        continue;
      }
      if (c == '&') {
        const auto entity = std::ranges::find_if(kEntities, [&](const auto& e) { return markup.substr(i).starts_with(e.first); });
        if (entity != kEntities.end()) {
          out += entity->second;
          i += entity->first.size();
          continue;
        }
      }
      out += c;
      ++i;
    }
    return out;
  }

  std::optional<std::string> extractText(const std::string& path) {
    switch (documentKind(path)) {
    case DocumentKind::Text:
      return plainText(path);
    case DocumentKind::Html: {
      const auto markup = readHead(path, kMaxMarkup);
      return markup ? std::optional(stripTags(*markup, kMaxText)) : std::nullopt;
    }
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
