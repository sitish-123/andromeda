#include "theme/toolkit_theme_files.h"

#include "core/log.h"
#include "theme/gtk_extensions.h"
#include "ui/style.h"
#include "util/file_utils.h"

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace toolkit_theme {

  namespace {

    using noctalia::theme::GtkToolkit;
    using noctalia::theme::gtkConfigDir;
    using noctalia::theme::kGtkExtensionIndexFile;

    constexpr Logger kLog("toolkit-theme");
    constexpr std::string_view kGtkImport = R"(@import url("andromeda.css");)";

    std::optional<std::string> readFile(const std::filesystem::path& path) {
      std::ifstream in(path, std::ios::binary);
      if (!in) {
        return std::nullopt;
      }
      std::ostringstream text;
      text << in.rdbuf();
      return text.str();
    }

    // Atomic replace; no write when the content is already there, so apps do not reload for nothing.
    void writeIfChanged(const std::filesystem::path& linkPath, const std::string& content) {
      if (readFile(linkPath) == content) {
        return;
      }
      std::error_code ec;
      // A user's symlink (e.g. into a dotfiles repo) stays a symlink: write its target.
      const std::filesystem::path path =
          std::filesystem::is_symlink(linkPath, ec) ? std::filesystem::weakly_canonical(linkPath, ec) : linkPath;
      std::filesystem::create_directories(path.parent_path(), ec);
      const std::filesystem::path tmp = path.string() + ".tmp";
      {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << content;
        out.flush();
        if (!out) {
          kLog.warn("write {} failed", tmp.string());
          out.close();
          std::filesystem::remove(tmp, ec);
          return;
        }
      }
      std::filesystem::rename(tmp, path, ec);
      if (ec) {
        kLog.warn("write {} failed: {}", path.string(), ec.message());
      }
    }

    // First match of `relative` in $XDG_DATA_HOME, then $XDG_DATA_DIRS.
    std::optional<std::filesystem::path> findData(std::string_view relative) {
      std::vector<std::string> dirs{FileUtils::expandXdgBaseDir("$XDG_DATA_HOME")};
      const char* env = std::getenv("XDG_DATA_DIRS");
      std::stringstream list(env != nullptr && *env != '\0' ? env : "/usr/local/share:/usr/share");
      for (std::string dir; std::getline(list, dir, ':');) {
        dirs.push_back(dir);
      }
      for (const std::string& dir : dirs) {
        const std::filesystem::path path = std::filesystem::path(dir) / relative;
        if (std::filesystem::exists(path)) {
          return path;
        }
      }
      return std::nullopt;
    }

    // Sets key=value in [section] of an INI file (section "" = before the first header); keeps everything else.
    void setIniKeys(
        const std::filesystem::path& path, std::string_view section,
        const std::vector<std::pair<std::string, std::string>>& keys
    ) {
      std::vector<std::string> lines;
      std::stringstream in(readFile(path).value_or(""));
      for (std::string line; std::getline(in, line);) {
        lines.push_back(line);
      }
      for (const auto& [key, value] : keys) {
        std::string current;
        std::optional<std::size_t> sectionEnd = section.empty() ? std::optional<std::size_t>{0} : std::nullopt;
        bool done = false;
        for (std::size_t i = 0; i < lines.size() && !done; ++i) {
          const std::string& line = lines[i];
          if (line.starts_with('[')) {
            if (current == section && sectionEnd.has_value()) {
              break;
            }
            current = line.substr(1, line.find(']') - 1);
            if (current == section) {
              sectionEnd = i + 1;
            }
            continue;
          }
          if (current == section) {
            sectionEnd = i + 1;
            if (line.starts_with(key + "=")) {
              lines[i] = key + "=" + value;
              done = true;
            }
          }
        }
        if (done) {
          continue;
        }
        if (!sectionEnd.has_value()) {
          lines.push_back(std::format("[{}]", section));
          sectionEnd = lines.size();
        }
        lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(*sectionEnd), key + "=" + value);
      }
      std::string out;
      for (const std::string& line : lines) {
        out += line + "\n";
      }
      writeIfChanged(path, out);
    }

    void writeGtk(GtkToolkit toolkit, std::string_view scheme, const std::string& accent, const std::string& onAccent) {
      const bool gtk4 = toolkit == GtkToolkit::Gtk4;
      const auto base = findData(std::format("andromeda/gtk/andromeda-gtk{}-{}.css", gtk4 ? 4 : 3, scheme));
      if (!base) {
        return;
      }
      const std::filesystem::path dir = gtkConfigDir(toolkit);
      std::string css = std::format("/* Written by the Andromeda shell; edits are replaced. */\n@import url(\"file://{}\");\n",
                                    base->string());
      if (std::filesystem::exists(dir / kGtkExtensionIndexFile)) {
        css += std::format("@import url(\"{}\");\n", kGtkExtensionIndexFile);
      }
      css += std::format(
          "@define-color accent_color {0};\n@define-color accent_bg_color {0};\n@define-color accent_fg_color {1};\n",
          accent, onAccent
      );
      if (gtk4) {
        css += std::format(":root {{\n  --accent-color: {0};\n  --accent-bg-color: {0};\n  --accent-fg-color: {1};\n}}\n",
                           accent, onAccent);
      }
      writeIfChanged(dir / "andromeda.css", css);
      // GTK reads only gtk.css: import ours once, first, so the user's own rules still win.
      const std::filesystem::path userCss = dir / "gtk.css";
      std::string existing = readFile(userCss).value_or("");
      // The retired GTK template imported noctalia.css; drop that line so its stale colors stop loading.
      constexpr std::string_view kOldImport = R"(@import url("noctalia.css");)";
      if (const auto at = existing.find(kOldImport); at != std::string::npos) {
        const auto end = existing.find('\n', at);
        existing.erase(at, end == std::string::npos ? std::string::npos : end - at + 1);
      }
      if (existing.find(kGtkImport) == std::string::npos) {
        existing = std::string(kGtkImport) + "\n" + existing;
      }
      writeIfChanged(userCss, existing);
    }

    std::string argb(std::uint32_t rgba) { return std::format("#ff{:06x}", rgba >> 8U); }

  } // namespace

  void remove() {
    for (const GtkToolkit toolkit : {GtkToolkit::Gtk3, GtkToolkit::Gtk4}) {
      const std::filesystem::path userCss = gtkConfigDir(toolkit) / "gtk.css";
      std::string existing = readFile(userCss).value_or("");
      if (const auto at = existing.find(kGtkImport); at != std::string::npos) {
        const auto end = existing.find('\n', at);
        existing.erase(at, end == std::string::npos ? std::string::npos : end - at + 1);
        writeIfChanged(userCss, existing);
      }
    }
  }

  bool write(std::string_view scheme, const Color& accent, const Color& onAccent) {
    const std::string accentHex = formatRgbHex(accent);
    const std::string onAccentHex = formatRgbHex(onAccent);
    if (!findData(std::format("andromeda/gtk/andromeda-gtk4-{}.css", scheme))) {
      kLog.debug("generated toolkit themes not installed; skipping");
      return false;
    }
    writeGtk(GtkToolkit::Gtk3, scheme, accentHex, onAccentHex);
    writeGtk(GtkToolkit::Gtk4, scheme, accentHex, onAccentHex);

    // qt6ct has no import: copy the scheme palette with the token accent replaced by the live one.
    if (const auto base = findData(std::format("qt6ct/colors/andromeda-qt-{}.conf", scheme))) {
      std::string palette = readFile(*base).value_or("");
      const std::string tokenAccent = argb(scheme == "light" ? Tokens::schemeLight.accent : Tokens::schemeDark.accent);
      const std::string liveAccent = "#ff" + accentHex.substr(1);
      for (std::size_t at = palette.find(tokenAccent); at != std::string::npos;
           at = palette.find(tokenAccent, at + liveAccent.size())) {
        palette.replace(at, tokenAccent.size(), liveAccent);
      }
      const std::filesystem::path colors = FileUtils::expandXdgBaseDir("$XDG_CONFIG_HOME/qt6ct/colors/andromeda.conf");
      writeIfChanged(colors, palette);
      setIniKeys(
          FileUtils::expandXdgBaseDir("$XDG_CONFIG_HOME/qt6ct/qt6ct.conf"), "Appearance",
          {{"color_scheme_path", colors.string()}, {"custom_palette", "true"}}
      );
    }

    // KDE apps read their colors from kdeglobals, not from the scheme file: merge the scheme's groups into it.
    const std::string schemeName = scheme == "light" ? "AndromedaLight" : "AndromedaDark";
    if (const auto base = findData(std::format("color-schemes/{}.colors", schemeName))) {
      const std::filesystem::path kdeglobals = FileUtils::expandXdgBaseDir("$XDG_CONFIG_HOME/kdeglobals");
      const std::uint32_t token = scheme == "light" ? Tokens::schemeLight.accent : Tokens::schemeDark.accent;
      const std::string tokenRgb = std::format("{},{},{}", token >> 24U, (token >> 16U) & 0xFFU, (token >> 8U) & 0xFFU);
      const std::string liveRgb = std::format(
          "{},{},{}", static_cast<int>(std::lround(accent.r * 255.0F)), static_cast<int>(std::lround(accent.g * 255.0F)),
          static_cast<int>(std::lround(accent.b * 255.0F))
      );
      std::string section;
      std::vector<std::pair<std::string, std::string>> keys;
      const auto flush = [&] {
        if (section.starts_with("Colors:") && !keys.empty()) {
          setIniKeys(kdeglobals, section, keys);
        }
        keys.clear();
      };
      std::stringstream lines(readFile(*base).value_or(""));
      for (std::string line; std::getline(lines, line);) {
        if (line.starts_with('[')) {
          flush();
          section = line.substr(1, line.find(']') - 1);
        } else if (const auto eq = line.find('='); eq != std::string::npos && !line.starts_with('#')) {
          std::string value = line.substr(eq + 1);
          keys.emplace_back(line.substr(0, eq), value == tokenRgb ? liveRgb : value);
        }
      }
      flush();
      setIniKeys(kdeglobals, "General", {{"ColorScheme", schemeName}, {"AccentColor", liveRgb}});
    }

    // fcitx5 switches light/dark itself from the portal color-scheme and reads the portal accent.
    setIniKeys(
        FileUtils::expandXdgBaseDir("$XDG_CONFIG_HOME/fcitx5/conf/classicui.conf"), "",
        {{"Theme", "andromeda-light"}, {"DarkTheme", "andromeda-dark"}, {"UseDarkTheme", "True"},
         {"UseAccentColor", "True"}}
    );
    return true;
  }

} // namespace toolkit_theme
