#include "launcher/web_search_provider.h"

#include "i18n/i18n.h"
#include "net/url_open.h"

#include <cstdio>

namespace {

  std::string percentEncode(std::string_view text) {
    std::string out;
    for (const unsigned char c : text) {
      if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_'
          || c == '.' || c == '~') {
        out.push_back(static_cast<char>(c));
      } else {
        char buf[4];
        std::snprintf(buf, sizeof(buf), "%%%02X", c);
        out += buf;
      }
    }
    return out;
  }

} // namespace

std::string WebSearchProvider::displayName() const { return i18n::tr("launcher.providers.web.title"); }

std::vector<LauncherResult> WebSearchProvider::query(std::string_view text) const {
  if (text.empty()) {
    return {};
  }
  LauncherResult result;
  result.id = std::string(text);
  result.title = i18n::tr("launcher.pill.search-web", "query", std::string(text));
  result.glyphName = std::string(defaultGlyphName());
  return {std::move(result)};
}

bool WebSearchProvider::activate(const LauncherResult& result) {
  return net::openInBrowser("https://duckduckgo.com/?q=" + percentEncode(result.id));
}
