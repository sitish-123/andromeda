#include "overview/app_icons.h"

#include "core/main_loop_worker.h"
#include "scene/text_buffer.h"

#include <array>
#include <cairo.h>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <librsvg/rsvg.h>
#include <optional>
#include <vector>

extern "C" {
#include <wlr/types/wlr_buffer.h>
}

namespace andromeda::ov {

  namespace {

    namespace fs = std::filesystem;
    using Surface = std::shared_ptr<cairo_surface_t>;

    std::string lower(std::string_view text) {
      std::string out(text);
      for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      }
      return out;
    }

    std::string env(const char* name, std::string_view fallback) {
      const char* value = std::getenv(name);
      return value != nullptr && *value != '\0' ? std::string(value) : std::string(fallback);
    }

    // XDG data dirs, user first.
    std::vector<fs::path> dataDirs() {
      std::vector<fs::path> dirs;
      dirs.emplace_back(env("XDG_DATA_HOME", env("HOME", "") + "/.local/share"));
      const std::string list = env("XDG_DATA_DIRS", "/usr/local/share:/usr/share");
      size_t start = 0;
      while (start <= list.size()) {
        const size_t end = std::min(list.find(':', start), list.size());
        if (end > start) {
          dirs.emplace_back(list.substr(start, end - start));
        }
        start = end + 1;
      }
      return dirs;
    }

    // The value of `key` in the file's first group, or empty.
    std::string keyValue(const fs::path& file, std::string_view key) {
      std::ifstream in(file);
      std::string line;
      bool first = true;
      while (std::getline(in, line)) {
        if (!line.empty() && line.front() == '[') {
          if (!first) {
            break;
          }
          first = false;
          continue;
        }
        if (line.size() > key.size() && line.compare(0, key.size(), key) == 0 && line[key.size()] == '=') {
          return line.substr(key.size() + 1);
        }
      }
      return {};
    }

    // The Icon= of the app's .desktop file: by file name, then by StartupWMClass; else the app id itself.
    std::string iconName(const std::string& appId, const std::vector<fs::path>& dirs) {
      for (const std::string& name : {appId, lower(appId)}) {
        for (const fs::path& dir : dirs) {
          const fs::path file = dir / "applications" / (name + ".desktop");
          std::error_code ec;
          if (fs::is_regular_file(file, ec)) {
            if (std::string icon = keyValue(file, "Icon"); !icon.empty()) {
              return icon;
            }
          }
        }
      }
      const std::string wanted = lower(appId);
      for (const fs::path& dir : dirs) {
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(dir / "applications", ec)) {
          if (entry.path().extension() == ".desktop" && lower(keyValue(entry.path(), "StartupWMClass")) == wanted) {
            if (std::string icon = keyValue(entry.path(), "Icon"); !icon.empty()) {
              return icon;
            }
          }
        }
      }
      return lower(appId);
    }

    std::string gtkIconTheme() {
      const fs::path settings = fs::path(env("XDG_CONFIG_HOME", env("HOME", "") + "/.config")) / "gtk-3.0/settings.ini";
      std::ifstream in(settings);
      std::string line;
      while (std::getline(in, line)) {
        if (line.starts_with("gtk-icon-theme-name")) {
          const size_t eq = line.find('=');
          if (eq != std::string::npos) {
            std::string value = line.substr(eq + 1);
            std::erase_if(value, [](char c) { return std::isspace(static_cast<unsigned char>(c)) != 0 || c == '"'; });
            return value;
          }
        }
      }
      return {};
    }

    std::optional<fs::path> findIcon(const std::string& name, const std::vector<fs::path>& dirs) {
      std::error_code ec;
      if (name.starts_with('/')) {
        return fs::is_regular_file(name, ec) ? std::optional<fs::path>(name) : std::nullopt;
      }
      std::vector<std::string> themes;
      if (std::string theme = gtkIconTheme(); !theme.empty()) {
        themes.push_back(std::move(theme));
      }
      themes.emplace_back("Adwaita");
      themes.emplace_back("hicolor");
      std::vector<fs::path> roots;
      roots.emplace_back(env("HOME", "") + "/.icons");
      for (const fs::path& dir : dirs) {
        roots.push_back(dir / "icons");
      }
      // Scalable first; then the larger sizes, which scale down cleanly.
      static constexpr std::array kSizes = {"scalable", "512x512", "256x256", "128x128", "96x96", "64x64", "48x48", "32x32"};
      for (const std::string& theme : themes) {
        for (const fs::path& root : roots) {
          if (!fs::is_directory(root / theme, ec)) {
            continue;
          }
          for (const char* size : kSizes) {
            for (const char* ext : {".svg", ".png"}) {
              const fs::path file = root / theme / size / "apps" / (name + ext);
              if (fs::is_regular_file(file, ec)) {
                return file;
              }
            }
          }
        }
      }
      for (const fs::path& dir : dirs) {
        for (const char* ext : {".svg", ".png"}) {
          const fs::path file = dir / "pixmaps" / (name + ext);
          if (fs::is_regular_file(file, ec)) {
            return file;
          }
        }
      }
      return std::nullopt;
    }

    Surface render(const fs::path& file, int px) {
      Surface out(cairo_image_surface_create(CAIRO_FORMAT_ARGB32, px, px), cairo_surface_destroy);
      cairo_t* cr = cairo_create(out.get());
      bool drawn = false;
      if (file.extension() == ".svg") {
        if (RsvgHandle* handle = rsvg_handle_new_from_file(file.c_str(), nullptr)) {
          const RsvgRectangle viewport{0, 0, static_cast<double>(px), static_cast<double>(px)};
          drawn = rsvg_handle_render_document(handle, cr, &viewport, nullptr) != 0;
          g_object_unref(handle);
        }
      } else {
        cairo_surface_t* png = cairo_image_surface_create_from_png(file.c_str());
        const int w = cairo_image_surface_get_width(png);
        const int h = cairo_image_surface_get_height(png);
        if (cairo_surface_status(png) == CAIRO_STATUS_SUCCESS && w > 0 && h > 0) {
          const double s = static_cast<double>(px) / std::max(w, h);
          cairo_scale(cr, s, s);
          cairo_set_source_surface(cr, png, (std::max(w, h) - w) / 2.0, (std::max(w, h) - h) / 2.0);
          cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
          cairo_paint(cr);
          drawn = true;
        }
        cairo_surface_destroy(png);
      }
      cairo_destroy(cr);
      cairo_surface_flush(out.get());
      return drawn ? out : nullptr;
    }

  } // namespace

  AppIcons::AppIcons(wl_event_loop* loop, std::function<void()> changed)
      : m_changed(std::move(changed)), m_worker(std::make_unique<MainLoopWorker>(loop)) {}

  AppIcons::~AppIcons() {
    m_worker.reset();
    for (auto& [key, buffer] : m_entries) {
      if (buffer != nullptr) {
        wlr_buffer_drop(buffer);
      }
    }
  }

  wlr_buffer* AppIcons::get(std::string_view appId, int px) {
    if (appId.empty() || px <= 0) {
      return nullptr;
    }
    std::string key = std::string(appId) + "@" + std::to_string(px);
    if (auto it = m_entries.find(key); it != m_entries.end()) {
      return it->second;
    }
    m_entries.emplace(key, nullptr);
    m_worker->post<Surface>(
        [id = std::string(appId), px]() -> Surface {
          const std::vector<fs::path> dirs = dataDirs();
          const std::optional<fs::path> file = findIcon(iconName(id, dirs), dirs);
          return file.has_value() ? render(*file, px) : nullptr;
        },
        [this, key](Surface surface) {
          if (surface != nullptr) {
            m_entries[key] = imageBuffer(surface.get());
            if (m_changed) {
              m_changed();
            }
          }
        }
    );
    return nullptr;
  }

} // namespace andromeda::ov
