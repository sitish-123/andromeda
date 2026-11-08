#include "smart_folders.h"

#include "core/deferred_call.h"
#include "core/files/file_info.h"
#include "index_search.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <format>
#include <fstream>
#include <thread>

#include <nlohmann/json.hpp>

namespace fs = std::filesystem;

namespace {
  std::string lower(std::string s) {
    std::ranges::transform(s, s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
  }

  // The index kind words, matched on the content type the name gives (the browser's kind chips do the same).
  bool kindMatches(const FileInfo& info, const std::string& kind) {
    const std::string_view t = info.contentType;
    const auto has = [&](std::string_view part) { return t.find(part) != std::string_view::npos; };
    if (kind.empty()) {
      return true;
    }
    if (info.isDir) {
      return false;
    }
    if (kind == "pdf") {
      return t == "application/pdf";
    }
    if (kind == "image") {
      return t.starts_with("image/");
    }
    if (kind == "doc") {
      return (t.starts_with("text/") && t != "text/csv") || has("msword") || has("wordprocessing") ||
             has("opendocument.text") || t == "application/rtf";
    }
    if (kind == "sheet") {
      return t == "text/csv" || has("spreadsheet") || has("ms-excel");
    }
    return true;
  }

  std::int64_t cutoffNs(int days) {
    if (days <= 0) {
      return 0;
    }
    const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                         std::chrono::system_clock::now().time_since_epoch()).count();
    return now - std::int64_t{days} * 24 * 3600 * 1'000'000'000;
  }

  bool matches(const SmartFolder& f, const FileInfo& info, const std::string& name, std::int64_t cutoff) {
    return (f.text.empty() || lower(name).find(lower(f.text)) != std::string::npos) && kindMatches(info, f.kind) &&
           info.mtimeNs >= cutoff;
  }

  void finish(smart_folders::Rows& rows) {
    std::ranges::sort(rows, [](const auto& a, const auto& b) { return a->mtimeNs > b->mtimeNs; });
    if (rows.size() > smart_folders::kMaxRows) {
      rows.resize(smart_folders::kMaxRows);
    }
  }

  const char* kindLabel(const std::string& kind) {
    return kind == "pdf" ? "PDF" : kind == "image" ? "Image" : kind == "doc" ? "Document" : kind == "sheet" ? "Spreadsheet" : "";
  }
} // namespace

std::string SmartFolder::sentence() const {
  std::vector<std::string> parts;
  if (!text.empty()) {
    parts.push_back(std::format("Name contains “{}”", text));
  }
  if (!kind.empty()) {
    parts.push_back(std::format("Kind is {}", kindLabel(kind)));
  }
  if (days == 1) {
    parts.emplace_back("Modified today");
  } else if (days > 0) {
    parts.push_back(days == 365 ? std::string("Modified in the last year")
                                : std::format("Modified in the last {} days", days));
  }
  parts.push_back(folder.empty() ? std::string("in This PC") : std::format("in {}", folder.filename().string()));
  std::string out;
  for (const auto& p : parts) {
    out += (out.empty() ? "" : " · ") + p;
  }
  return out;
}

namespace smart_folders {

  fs::path configPath() {
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    const char* home = std::getenv("HOME");
    const fs::path base = xdg != nullptr && *xdg != '\0' ? fs::path(xdg) : fs::path(home != nullptr ? home : "") / ".config";
    return base / "andromeda" / "files" / "smart-folders.json";
  }

  std::vector<SmartFolder> load() {
    std::vector<SmartFolder> out;
    std::ifstream in(configPath());
    if (!in) {
      return out;
    }
    const auto doc = nlohmann::json::parse(in, nullptr, false);
    if (!doc.is_array()) {
      return out;
    }
    for (const auto& j : doc) {
      if (!j.is_object() || j.value("name", "").empty()) {
        continue;
      }
      out.push_back({.name = j.value("name", ""), .text = j.value("text", ""), .kind = j.value("kind", ""),
                     .days = j.value("days", 0), .folder = fs::path(j.value("folder", ""))});
    }
    return out;
  }

  bool save(const std::vector<SmartFolder>& folders) {
    nlohmann::json doc = nlohmann::json::array();
    for (const auto& f : folders) {
      doc.push_back({{"name", f.name}, {"text", f.text}, {"kind", f.kind}, {"days", f.days}, {"folder", f.folder.string()}});
    }
    const fs::path path = configPath();
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    const fs::path tmp = path.string() + ".tmp";
    {
      std::ofstream out(tmp, std::ios::trunc);
      out << doc.dump(2) << '\n';
      if (!out) {
        return false;
      }
    }
    fs::rename(tmp, path, ec);
    return !ec;
  }

  void run(const SmartFolder& folder, IndexSearch* index, std::weak_ptr<bool> alive, std::function<void(Rows)> done) {
    const std::int64_t cutoff = cutoffNs(folder.days);
    if (folder.folder.empty() && index != nullptr && IndexSearch::available()) {
      index->query(folder.text, folder.kind, [folder, cutoff, alive, done](const std::string&, Rows rows) {
        if (alive.expired()) {
          return;
        }
        std::erase_if(rows, [&](const auto& r) { return r->mtimeNs < cutoff; });
        finish(rows);
        done(std::move(rows));
      });
      return;
    }
    const char* home = std::getenv("HOME");
    const fs::path root = folder.folder.empty() ? fs::path(home != nullptr ? home : "/") : folder.folder;
    std::thread([folder, root, cutoff, alive, done = std::move(done)] {
      Rows rows;
      std::error_code ec;
      std::size_t seen = 0;
      fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
      for (const fs::recursive_directory_iterator end; !ec && it != end && seen < kWalkLimit; it.increment(ec), ++seen) {
        if (alive.expired()) {
          return;
        }
        const std::string name = it->path().filename().string();
        std::error_code typeEc;
        if (name.starts_with('.')) {
          if (it->is_directory(typeEc)) {
            it.disable_recursion_pending();
          }
          continue;
        }
        auto info = statFile(it->path());
        if (info && matches(folder, *info, name, cutoff)) {
          info->name = it->path().string();
          rows.push_back(std::make_shared<const FileInfo>(std::move(*info)));
        }
      }
      finish(rows);
      DeferredCall::callLater([alive, done, rows = std::move(rows)]() mutable {
        if (!alive.expired()) {
          done(std::move(rows));
        }
      });
    }).detach();
  }

} // namespace smart_folders
