#include "system/default_apps.h"

#include "core/log.h"
#include "system/desktop_entry.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <fstream>
#include <optional>

namespace default_apps {

  namespace {

    constexpr Logger kLog("default-apps");

    constexpr std::array<CategorySpec, 9> kCategories{{
        {Category::WebBrowser, "x-scheme-handler/http", "WebBrowser"},
        {Category::Mail, "x-scheme-handler/mailto", "Email"},
        {Category::FileManager, "inode/directory", "FileManager"},
        {Category::Terminal, "x-scheme-handler/terminal", "TerminalEmulator"},
        {Category::TextEditor, "text/plain", "TextEditor"},
        {Category::ImageViewer, "image/png", "Viewer"},
        {Category::VideoPlayer, "video/mp4", "Player"},
        {Category::MusicPlayer, "audio/mpeg", "Player"},
        {Category::PdfViewer, "application/pdf", "Viewer"},
    }};

    std::filesystem::path configHome() {
      const char* home = std::getenv("HOME");
      std::filesystem::path base =
          home != nullptr && home[0] != '\0' ? std::filesystem::path(home) / ".config" : std::filesystem::path("/tmp");
      if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg != nullptr && xdg[0] != '\0') {
        const std::filesystem::path candidate(xdg);
        if (candidate.is_absolute()) {
          base = candidate;
        }
      }
      return base;
    }

    std::vector<std::string> splitMimeList(std::string_view value) {
      std::vector<std::string> parts;
      for (std::size_t start = 0; start <= value.size();) {
        const std::size_t sep = value.find(';', start);
        std::string_view part = value.substr(start, sep - start);
        if (!part.empty()) {
          parts.emplace_back(part);
        }
        if (sep == std::string_view::npos) {
          break;
        }
        start = sep + 1;
      }
      return parts;
    }

  } // namespace

  std::span<const CategorySpec> categories() { return kCategories; }

  std::filesystem::path mimeAppsListPath() { return configHome() / "mimeapps.list"; }

  std::string currentDefault(const std::filesystem::path& path, Category category) {
    const auto it = std::ranges::find(kCategories, category, &CategorySpec::category);
    if (it == kCategories.end()) {
      return {};
    }
    std::ifstream in(path);
    std::string line;
    bool inSection = false;
    while (std::getline(in, line)) {
      if (!line.empty() && line.front() == '[') {
        inSection = line == "[Default Applications]";
        continue;
      }
      if (!inSection) {
        continue;
      }
      const std::size_t eq = line.find('=');
      if (eq == std::string::npos || line.compare(0, eq, it->mimeType) != 0) {
        continue;
      }
      const std::vector<std::string> ids = splitMimeList(std::string_view(line).substr(eq + 1));
      return ids.empty() ? std::string() : ids.front();
    }
    return {};
  }

  std::vector<const DesktopEntry*> candidatesForCategory(std::span<const DesktopEntry> entries, Category category) {
    const auto it = std::ranges::find(kCategories, category, &CategorySpec::category);
    if (it == kCategories.end()) {
      return {};
    }
    std::vector<const DesktopEntry*> byMime;
    std::vector<const DesktopEntry*> byCategory;
    for (const DesktopEntry& entry : entries) {
      if (entry.exec.empty()) {
        continue;
      }
      if (!entry.mimeTypes.empty()) {
        for (const std::string& mime : splitMimeList(entry.mimeTypes)) {
          if (mime == it->mimeType) {
            byMime.push_back(&entry);
            break;
          }
        }
      } else if (entry.categories.find(std::string(it->mainCategory)) != std::string::npos) {
        byCategory.push_back(&entry);
      }
    }
    return byMime.empty() ? byCategory : byMime;
  }

  bool setDefault(const std::filesystem::path& path, Category category, const std::string& desktopId) {
    const auto it = std::ranges::find(kCategories, category, &CategorySpec::category);
    if (it == kCategories.end()) {
      return false;
    }
    std::vector<std::string> lines;
    {
      std::ifstream in(path);
      std::string line;
      while (std::getline(in, line)) {
        lines.push_back(line);
      }
    }

    std::optional<std::size_t> sectionStart;
    std::optional<std::size_t> sectionEnd; // exclusive
    std::optional<std::size_t> keyLine;
    for (std::size_t i = 0; i < lines.size(); ++i) {
      if (!lines[i].empty() && lines[i].front() == '[') {
        if (sectionStart.has_value() && !sectionEnd.has_value()) {
          sectionEnd = i;
        }
        if (lines[i] == "[Default Applications]") {
          sectionStart = i;
        }
        continue;
      }
      if (sectionStart.has_value() && !sectionEnd.has_value()) {
        const std::size_t eq = lines[i].find('=');
        if (eq != std::string::npos && lines[i].compare(0, eq, it->mimeType) == 0) {
          keyLine = i;
        }
      }
    }
    if (sectionStart.has_value() && !sectionEnd.has_value()) {
      sectionEnd = lines.size();
    }

    const std::string newLine = std::string(it->mimeType) + "=" + desktopId + ";";
    if (keyLine.has_value()) {
      lines[*keyLine] = newLine;
    } else if (sectionStart.has_value()) {
      lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(*sectionEnd), newLine);
    } else {
      if (!lines.empty()) {
        lines.emplace_back();
      }
      lines.emplace_back("[Default Applications]");
      lines.push_back(newLine);
    }

    std::ofstream out(path, std::ios::trunc);
    if (!out) {
      kLog.warn("cannot write {}", path.string());
      return false;
    }
    for (const std::string& line : lines) {
      out << line << '\n';
    }
    return true;
  }

} // namespace default_apps
