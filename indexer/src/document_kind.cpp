#include "extract.h"
#include "file_kind.h"

#include <algorithm>
#include <cctype>
#include <iterator>
#include <utility>

namespace andromeda::index {

  namespace {
    // Documents only: source and config files are found by name; their text would be mostly vendored code
    // (a home here: 168k such files, 1.7 GB of text, against 6.8k documents).
    constexpr std::string_view kTextExtensions[] = {"txt", "md",  "markdown", "rst", "org",
                                                    "tex", "csv", "tsv",      "srt", "vtt"};

    constexpr std::pair<std::string_view, FileKind> kKindOf[] = {
        {"pdf", FileKind::Pdf},
        {"png", FileKind::Image},     {"jpg", FileKind::Image},    {"jpeg", FileKind::Image},  {"gif", FileKind::Image},
        {"webp", FileKind::Image},    {"avif", FileKind::Image},   {"heic", FileKind::Image},  {"heif", FileKind::Image},
        {"jxl", FileKind::Image},     {"bmp", FileKind::Image},    {"tif", FileKind::Image},   {"tiff", FileKind::Image},
        {"svg", FileKind::Image},     {"ico", FileKind::Image},    {"psd", FileKind::Image},   {"xcf", FileKind::Image},
        {"dng", FileKind::Image},     {"cr2", FileKind::Image},    {"cr3", FileKind::Image},   {"nef", FileKind::Image},
        {"arw", FileKind::Image},     {"raf", FileKind::Image},    {"orf", FileKind::Image},
        {"mp4", FileKind::Video},     {"mkv", FileKind::Video},    {"webm", FileKind::Video},  {"mov", FileKind::Video},
        {"avi", FileKind::Video},     {"m4v", FileKind::Video},    {"wmv", FileKind::Video},   {"flv", FileKind::Video},
        {"mpg", FileKind::Video},     {"mpeg", FileKind::Video},   {"3gp", FileKind::Video},   {"ogv", FileKind::Video},
        {"mp3", FileKind::Audio},     {"flac", FileKind::Audio},   {"ogg", FileKind::Audio},   {"oga", FileKind::Audio},
        {"opus", FileKind::Audio},    {"m4a", FileKind::Audio},    {"aac", FileKind::Audio},   {"wav", FileKind::Audio},
        {"wma", FileKind::Audio},     {"aiff", FileKind::Audio},   {"aif", FileKind::Audio},   {"mid", FileKind::Audio},
        {"midi", FileKind::Audio},
        {"doc", FileKind::Doc},       {"docx", FileKind::Doc},     {"odt", FileKind::Doc},     {"rtf", FileKind::Doc},
        {"txt", FileKind::Doc},       {"md", FileKind::Doc},       {"markdown", FileKind::Doc}, {"rst", FileKind::Doc},
        {"org", FileKind::Doc},       {"tex", FileKind::Doc},      {"epub", FileKind::Doc},    {"pages", FileKind::Doc},
        {"xls", FileKind::Sheet},     {"xlsx", FileKind::Sheet},   {"ods", FileKind::Sheet},   {"csv", FileKind::Sheet},
        {"tsv", FileKind::Sheet},     {"numbers", FileKind::Sheet},
        {"ppt", FileKind::Slides},    {"pptx", FileKind::Slides},  {"odp", FileKind::Slides},  {"key", FileKind::Slides},
    };

    constexpr std::pair<std::string_view, FileKind> kKindWords[] = {
        {"folder", FileKind::Folder}, {"pdf", FileKind::Pdf},   {"image", FileKind::Image},
        {"video", FileKind::Video},   {"audio", FileKind::Audio}, {"doc", FileKind::Doc},
        {"sheet", FileKind::Sheet},   {"slides", FileKind::Slides},
    };
  } // namespace

  FileKind fileKind(std::string_view name, bool isDir) {
    if (isDir) {
      return FileKind::Folder;
    }
    const std::string ext = extensionOf(name);
    const auto* it = std::ranges::find(kKindOf, ext, &std::pair<std::string_view, FileKind>::first);
    return it != std::end(kKindOf) ? it->second : FileKind::Other;
  }

  std::optional<FileKind> parseKind(std::string_view word) {
    const auto* it = std::ranges::find(kKindWords, word, &std::pair<std::string_view, FileKind>::first);
    return it != std::end(kKindWords) ? std::optional(it->second) : std::nullopt;
  }

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
