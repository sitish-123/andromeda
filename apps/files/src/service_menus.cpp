#include "service_menus.h"

#include "core/files/file_info.h"

#include <algorithm>
#include <gio/gio.h>
#include <glib.h>

namespace fs = std::filesystem;

namespace {
  std::vector<fs::path> menuDirs() {
    std::vector<fs::path> dirs;
    const auto add = [&](const fs::path& base) {
      dirs.push_back(base / "kio" / "servicemenus");
      dirs.push_back(base / "andromeda-files" / "servicemenus");
    };
    add(g_get_user_data_dir());
    for (const gchar* const* dir = g_get_system_data_dirs(); dir != nullptr && *dir != nullptr; ++dir) {
      add(*dir);
    }
    return dirs;
  }

  std::vector<std::string> list(GKeyFile* file, const char* group, const char* key) {
    std::vector<std::string> out;
    gsize n = 0;
    gchar** values = g_key_file_get_string_list(file, group, key, &n, nullptr);
    for (gsize i = 0; i < n; ++i) {
      if (values[i][0] != '\0') {
        out.emplace_back(values[i]);
      }
    }
    g_strfreev(values);
    return out;
  }

  std::string text(GKeyFile* file, const char* group, const char* key) {
    gchar* value = g_key_file_get_locale_string(file, group, key, nullptr, nullptr);
    std::string out = value != nullptr ? value : "";
    g_free(value);
    return out;
  }

  // KDE patterns: exact type, "type/*", all/all (files and folders), all/allfiles (files), octet-stream (any file).
  bool matches(const std::vector<std::string>& patterns, const FileInfo& item) {
    return std::ranges::any_of(patterns, [&](const std::string& p) {
      if (p == "all/all") {
        return true;
      }
      if (p == "all/allfiles" || p == "application/octet-stream") {
        return !item.isDir;
      }
      if (p.ends_with("/*")) {
        return item.contentType.starts_with(p.substr(0, p.size() - 1));
      }
      return p == item.contentType;
    });
  }

  std::string uriOf(const fs::path& path) {
    gchar* uri = g_filename_to_uri(path.c_str(), nullptr, nullptr);
    std::string out = uri != nullptr ? uri : "";
    g_free(uri);
    return out;
  }

  std::string quoted(const std::string& s) {
    gchar* q = g_shell_quote(s.c_str());
    std::string out = q;
    g_free(q);
    return out;
  }
} // namespace

namespace service_menus {
  std::vector<Action> forItems(const std::vector<fs::path>& paths) {
    std::vector<FileInfo> items;
    for (const fs::path& p : paths) {
      if (auto info = statFile(p)) {
        items.push_back(std::move(*info));
      }
    }
    std::vector<Action> out;
    if (items.empty()) {
      return out;
    }
    std::vector<fs::path> files;
    for (const fs::path& dir : menuDirs()) {
      std::error_code ec;
      for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (entry.path().extension() == ".desktop") {
          files.push_back(entry.path());
        }
      }
    }
    std::ranges::sort(files, [](const fs::path& a, const fs::path& b) { return a.filename() < b.filename(); });
    for (const fs::path& path : files) {
      GKeyFile* file = g_key_file_new();
      if (g_key_file_load_from_file(file, path.c_str(), G_KEY_FILE_NONE, nullptr)) {
        const char* entry = "Desktop Entry";
        const auto types = list(file, entry, "MimeType");
        const bool all = std::ranges::all_of(items, [&](const FileInfo& item) { return matches(types, item); });
        if (all && !types.empty()) {
          const std::string submenu = text(file, entry, "X-KDE-Submenu");
          for (const std::string& id : list(file, entry, "Actions")) {
            const std::string group = "Desktop Action " + id;
            Action action{.name = text(file, group.c_str(), "Name"), .icon = text(file, group.c_str(), "Icon"),
                          .exec = text(file, group.c_str(), "Exec"), .submenu = submenu};
            if (!action.name.empty() && !action.exec.empty()) {
              out.push_back(std::move(action));
            }
          }
        }
      }
      g_key_file_free(file);
    }
    return out;
  }

  bool run(const Action& action, const std::vector<fs::path>& paths, const fs::path& folder) {
    if (paths.empty()) {
      return false;
    }
    // Field codes expand to shell-quoted words, then the whole line parses as one argv (no shell runs).
    std::string line;
    for (std::size_t i = 0; i < action.exec.size(); ++i) {
      if (action.exec[i] != '%' || i + 1 == action.exec.size()) {
        line += action.exec[i];
        continue;
      }
      const char code = action.exec[++i];
      if (code == 'f') {
        line += quoted(paths.front().string());
      } else if (code == 'u') {
        line += quoted(uriOf(paths.front()));
      } else if (code == 'F' || code == 'U') {
        for (std::size_t k = 0; k < paths.size(); ++k) {
          line += (k > 0 ? " " : "") + quoted(code == 'F' ? paths[k].string() : uriOf(paths[k]));
        }
      } else if (code == '%') {
        line += '%';
      }
    }
    gint argc = 0;
    gchar** argv = nullptr;
    if (!g_shell_parse_argv(line.c_str(), &argc, &argv, nullptr)) {
      return false;
    }
    const gboolean ok = g_spawn_async(folder.c_str(), argv, nullptr, G_SPAWN_SEARCH_PATH, nullptr, nullptr, nullptr, nullptr);
    g_strfreev(argv);
    return ok != FALSE;
  }
} // namespace service_menus
