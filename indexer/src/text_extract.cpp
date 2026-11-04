#include "extract.h"

#include <iconv.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <fstream>

// Text and HTML documents, read in the daemon: no library beyond libc, and no helper process to start.
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
      const iconv_t cd = iconv_open("UTF-8", encoding);
      if (cd == reinterpret_cast<iconv_t>(-1)) {
        return std::nullopt;
      }
      std::size_t inLeft = data->size() & ~std::size_t{3};
      std::string text(inLeft * 2, '\0'); // UTF-16 needs at most 3/2, UTF-32 at most 1 byte out per byte in
      char* in = data->data();
      char* out = text.data();
      std::size_t outLeft = text.size();
      const std::size_t converted = iconv(cd, &in, &inLeft, &out, &outLeft);
      iconv_close(cd);
      if (converted == static_cast<std::size_t>(-1) && errno != EINVAL) {
        return std::nullopt;
      }
      text.resize(text.size() - outLeft);
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

  std::optional<std::string> extractPlain(const std::string& path) {
    if (documentKind(path) == DocumentKind::Text) {
      return plainText(path);
    }
    const auto markup = readHead(path, kMaxMarkup);
    return markup ? std::optional(stripTags(*markup, kMaxText)) : std::nullopt;
  }

} // namespace andromeda::index
