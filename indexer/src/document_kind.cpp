#include "extract.h"

#include <algorithm>
#include <cctype>
#include <iterator>

namespace andromeda::index {

  namespace {
    // Documents only: source and config files are found by name; their text would be mostly vendored code
    // (a home here: 168k such files, 1.7 GB of text, against 6.8k documents).
    constexpr std::string_view kTextExtensions[] = {"txt", "md",  "markdown", "rst", "org",
                                                    "tex", "csv", "tsv",      "srt", "vtt"};

  } // namespace

  std::string extensionOf(std::string_view name) {
    const std::size_t dot = name.rfind('.');
    if (dot == std::string_view::npos || dot == 0 || dot + 1 == name.size()) {
      return {};
    }
    std::string ext(name.substr(dot + 1));
    for (char& c : ext) {
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return ext;
  }

  DocumentKind documentKind(std::string_view name) {
    const std::string ext = extensionOf(name);
    if (ext.empty()) {
      return DocumentKind::None;
    }
    if (std::ranges::find(kTextExtensions, ext) != std::end(kTextExtensions)) {
      return DocumentKind::Text;
    }
    if (ext == "html" || ext == "htm" || ext == "xhtml") {
      return DocumentKind::Html;
    }
    if (ext == "pdf") {
      return DocumentKind::Pdf;
    }
    if (ext == "odt"
        || ext == "ods"
        || ext == "odp"
        || ext == "docx"
        || ext == "xlsx"
        || ext == "pptx"
        || ext == "epub") {
      return DocumentKind::Zip;
    }
    return DocumentKind::None;
  }

  bool extractable(std::string_view name) { return documentKind(name) != DocumentKind::None; }

} // namespace andromeda::index
