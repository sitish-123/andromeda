#include "view_store.h"

#include "state_file.h"
#include "ui/files/file_cells.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <format>
#include <fstream>
#include <glib.h>
#include <sys/xattr.h>
#include <vector>

namespace fs = std::filesystem;

namespace {
  constexpr const char* kXattr = "user.andromeda.view";
  constexpr std::size_t kMaxValue = 512;

  fs::path stateDir() { return fs::path(g_get_user_state_dir()) / "andromeda" / "files"; }
  fs::path fallbackFile() { return stateDir() / "views.tsv"; }

  // Paths may hold tabs, newlines and backslashes: escape them so each folder stays one line.
  std::string escape(const std::string& in) {
    std::string out;
    for (char c : in) {
      out += c == '\\' ? std::string("\\\\") : c == '\t' ? std::string("\\t") : c == '\n' ? std::string("\\n") : std::string(1, c);
    }
    return out;
  }

  std::string unescape(const std::string& in) {
    std::string out;
    for (std::size_t i = 0; i < in.size(); ++i) {
      if (in[i] == '\\' && i + 1 < in.size()) {
        const char n = in[++i];
        out += n == 't' ? '\t' : n == 'n' ? '\n' : n;
      } else {
        out += in[i];
      }
    }
    return out;
  }

  std::vector<std::pair<std::string, std::string>> readFallback() {
    std::vector<std::pair<std::string, std::string>> out;
    std::ifstream in(fallbackFile());
    for (std::string line; std::getline(in, line);) {
      const auto tab = line.find('\t');
      if (tab != std::string::npos) {
        out.emplace_back(unescape(line.substr(0, tab)), line.substr(tab + 1));
      }
    }
    return out;
  }

  void writeFallback(const std::vector<std::pair<std::string, std::string>>& rows) {
    std::string text;
    for (const auto& [path, value] : rows) {
      text += escape(path) + '\t' + value + '\n';
    }
    writeStateFile(fallbackFile(), text);
  }
} // namespace

namespace view_store {
  std::optional<std::string> read(const fs::path& folder) {
    char buf[kMaxValue];
    const ssize_t n = ::getxattr(folder.c_str(), kXattr, buf, sizeof(buf));
    if (n > 0) {
      return std::string(buf, static_cast<std::size_t>(n));
    }
    for (const auto& [path, value] : readFallback()) {
      if (path == folder.string()) {
        return value;
      }
    }
    return std::nullopt;
  }

  void write(const fs::path& folder, const std::string& value) {
    auto rows = readFallback();
    const auto it = std::ranges::find_if(rows, [&](const auto& r) { return r.first == folder.string(); });
    if (::setxattr(folder.c_str(), kXattr, value.data(), value.size(), 0) == 0) {
      if (it != rows.end()) { // the xattr wins from now on: drop the stale fallback line
        rows.erase(it);
        writeFallback(rows);
      }
      return;
    }
    if (it != rows.end()) {
      it->second = value;
    } else {
      rows.emplace_back(folder.string(), value);
    }
    writeFallback(rows);
  }

  std::string readDefault() {
    std::ifstream in(stateDir() / "view-default");
    std::string value;
    std::getline(in, value);
    return value;
  }

  void writeDefault(const std::string& value) {
    std::error_code ec;
    fs::create_directories(stateDir(), ec);
    std::ofstream(stateDir() / "view-default", std::ios::trunc) << value << '\n';
  }
} // namespace view_store

namespace {
  constexpr std::array<std::pair<const char*, std::uint8_t>, 4> kViews{
      {{"icons", 0}, {"list", 1}, {"columns", 2}, {"gallery", 3}}};
  constexpr std::array<std::pair<const char*, FileSortKey>, 5> kSorts{{{"name", FileSortKey::Name},
                                                                       {"modified", FileSortKey::Modified},
                                                                       {"added", FileSortKey::Added},
                                                                       {"size", FileSortKey::Size},
                                                                       {"kind", FileSortKey::Kind}}};

  constexpr std::array<std::pair<std::string_view, FileGroupBy>, 5> kGroups{{{"none", FileGroupBy::None},
                                                                             {"kind", FileGroupBy::Kind},
                                                                             {"date", FileGroupBy::Date},
                                                                             {"size", FileGroupBy::Size},
                                                                             {"tags", FileGroupBy::Tags}}};
  constexpr std::array<std::pair<std::string_view, std::uint8_t>, 3> kColumns{{{"dim", file_columns::kDimensions},
                                                                              {"dur", file_columns::kDuration},
                                                                              {"art", file_columns::kArtist}}};

  // Inside an XDG user folder (or the folder itself).
  bool within(const fs::path& folder, GUserDirectory dir) {
    const char* root = g_get_user_special_dir(dir);
    if (root == nullptr || fs::path(root) == fs::path(g_get_home_dir())) {
      return false;
    }
    const std::string base = fs::path(root).lexically_normal().string();
    const std::string path = folder.lexically_normal().string();
    return path == base || path.starts_with(base + "/");
  }
} // namespace

namespace view_store {
  std::string encode(const FileBrowserViewState& view) {
    std::string out = "v=";
    for (const auto& [name, v] : kViews) {
      out += v == view.view ? name : "";
    }
    out += std::format(";z={}", static_cast<int>(view.iconSize));
    for (const auto& [name, key] : kSorts) {
      out += key == view.sort.key ? std::format(";s={}", name) : "";
    }
    out += std::format(";d={};h={}", view.sort.descending ? 1 : 0, view.showHidden ? 1 : 0);
    for (const auto& [name, group] : kGroups) {
      out += group == view.group ? std::format(";g={}", name) : "";
    }
    out += std::format(";x={};c=", view.extensions ? 1 : 0);
    bool first = true;
    for (const auto& [name, bit] : kColumns) {
      if ((view.columns & bit) != 0) {
        out += first ? "" : ",";
        out += name;
        first = false;
      }
    }
    return out;
  }

  FileBrowserViewState decode(const std::string& text) {
    FileBrowserViewState view;
    std::size_t start = 0;
    while (start < text.size()) {
      const std::size_t end = std::min(text.find(';', start), text.size());
      const std::string part = text.substr(start, end - start);
      start = end + 1;
      const auto eq = part.find('=');
      if (eq == std::string::npos) {
        continue;
      }
      const std::string key = part.substr(0, eq);
      const std::string value = part.substr(eq + 1);
      if (key == "v") {
        for (const auto& [name, v] : kViews) {
          view.view = value == name ? v : view.view;
        }
      } else if (key == "z") {
        int z = 0;
        std::from_chars(value.data(), value.data() + value.size(), z);
        view.iconSize = static_cast<float>(std::clamp(z, 0, 512));
      } else if (key == "s") {
        for (const auto& [name, k] : kSorts) {
          view.sort.key = value == name ? k : view.sort.key;
        }
      } else if (key == "d") {
        view.sort.descending = value == "1";
      } else if (key == "h") {
        view.showHidden = value == "1";
      } else if (key == "g") {
        for (const auto& [name, group] : kGroups) {
          view.group = value == name ? group : view.group;
        }
      } else if (key == "x") {
        view.extensions = value != "0";
      } else if (key == "c") {
        view.columns = 0;
        for (const auto& [name, bit] : kColumns) {
          const std::string_view list = value;
          for (std::size_t at = 0; at <= list.size();) {
            const std::size_t comma = std::min(list.find(',', at), list.size());
            view.columns |= list.substr(at, comma - at) == name ? bit : 0;
            at = comma + 1;
          }
        }
      }
    }
    return view;
  }

  FileBrowserViewState forFolder(const fs::path& folder) {
    if (const auto own = read(folder)) {
      return decode(*own);
    }
    constexpr float kMediaIcon = 96.0F; // Pictures and Videos open with large previews
    if (within(folder, G_USER_DIRECTORY_PICTURES) || within(folder, G_USER_DIRECTORY_VIDEOS)) {
      return {.view = 0, .iconSize = kMediaIcon};
    }
    if (within(folder, G_USER_DIRECTORY_DOWNLOAD)) {
      return {.view = 1, .sort = {.key = FileSortKey::Added, .descending = true}};
    }
    if (within(folder, G_USER_DIRECTORY_MUSIC)) {
      return {.view = 1, .columns = static_cast<std::uint8_t>(file_columns::kArtist | file_columns::kDuration)};
    }
    const std::string fallback = readDefault();
    return fallback.empty() ? FileBrowserViewState{} : decode(fallback);
  }
} // namespace view_store
