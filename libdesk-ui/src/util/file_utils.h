#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <vector>

namespace FileUtils {

  [[nodiscard]] constexpr std::filesystem::perms privateFileMode() {
    return std::filesystem::perms::owner_read | std::filesystem::perms::owner_write;
  }

  [[nodiscard]] constexpr std::filesystem::perms privateDirectoryMode() {
    return privateFileMode() | std::filesystem::perms::owner_exec;
  }

  inline bool setPrivateFilePermissions(const std::filesystem::path& path, std::error_code& ec) {
    std::filesystem::permissions(path, privateFileMode(), std::filesystem::perm_options::replace, ec);
    return !ec;
  }

  inline bool setPrivateDirectoryPermissions(const std::filesystem::path& path, std::error_code& ec) {
    std::filesystem::permissions(path, privateDirectoryMode(), std::filesystem::perm_options::replace, ec);
    return !ec;
  }

  inline bool createPrivateDirectories(const std::filesystem::path& path, std::error_code& ec) {
    std::filesystem::create_directories(path, ec);
    if (ec) {
      return false;
    }
    return setPrivateDirectoryPermissions(path, ec);
  }

  [[nodiscard]] inline std::filesystem::path expandUserPath(const std::string& path) {
    if (path.empty() || path[0] != '~') {
      return std::filesystem::path(path);
    }
    const char* home = std::getenv("HOME");
    if (home == nullptr || home[0] == '\0') {
      return std::filesystem::path(path);
    }
    if (path.size() == 1) {
      return std::filesystem::path(home);
    }
    if (path[1] == '/') {
      return std::filesystem::path(home) / path.substr(2);
    }
    return std::filesystem::path(path);
  }

  // Expands $NAME and ${NAME} environment-variable references. NAME matches
  // [A-Za-z_][A-Za-z0-9_]*; undefined variables expand to the empty string. A
  // lone '$' or '$' followed by a non-name character is left verbatim. This is a
  // deliberate, minimal substitution — never wordexp()/shell expansion, which
  // would run command substitution on config input.
  [[nodiscard]] inline std::string expandEnvVars(std::string_view in) {
    std::string out;
    out.reserve(in.size());
    const auto isNameStart = [](char c) { return (std::isalpha(static_cast<unsigned char>(c)) != 0) || c == '_'; };
    const auto isNameChar = [](char c) { return (std::isalnum(static_cast<unsigned char>(c)) != 0) || c == '_'; };

    for (std::size_t i = 0; i < in.size();) {
      if (in[i] != '$') {
        out.push_back(in[i]);
        ++i;
        continue;
      }
      // in[i] == '$'
      if (i + 1 < in.size() && in[i + 1] == '{') {
        const std::size_t close = in.find('}', i + 2);
        if (close != std::string_view::npos) {
          const std::string name(in.substr(i + 2, close - (i + 2)));
          if (const char* val = std::getenv(name.c_str()); val != nullptr) {
            out.append(val);
          }
          i = close + 1;
          continue;
        }
        // No closing brace — emit '$' verbatim and continue.
        out.push_back('$');
        ++i;
        continue;
      }
      if (i + 1 < in.size() && isNameStart(in[i + 1])) {
        std::size_t j = i + 1;
        while (j < in.size() && isNameChar(in[j])) {
          ++j;
        }
        const std::string name(in.substr(i + 1, j - (i + 1)));
        if (const char* val = std::getenv(name.c_str()); val != nullptr) {
          out.append(val);
        }
        i = j;
        continue;
      }
      // Lone '$' — leave verbatim.
      out.push_back('$');
      ++i;
    }
    return out;
  }

  // Resolves the user's standard Pictures directory. The environment override is
  // useful for shells that export xdg-user-dirs values; the config-file lookup
  // covers the normal xdg-user-dirs setup.
  [[nodiscard]] inline std::filesystem::path defaultPicturesDirectory() {
    const auto containsUndefinedVariable = [](std::string_view raw) {
      const auto isNameStart = [](char c) { return (std::isalpha(static_cast<unsigned char>(c)) != 0) || c == '_'; };
      const auto isNameChar = [](char c) { return (std::isalnum(static_cast<unsigned char>(c)) != 0) || c == '_'; };

      for (std::size_t i = 0; i < raw.size();) {
        if (raw[i] != '$') {
          ++i;
          continue;
        }
        if (i + 1 < raw.size() && raw[i + 1] == '{') {
          const std::size_t close = raw.find('}', i + 2);
          if (close == std::string_view::npos) {
            ++i;
            continue;
          }
          const std::string name(raw.substr(i + 2, close - (i + 2)));
          const char* value = std::getenv(name.c_str());
          if (value == nullptr || value[0] == '\0') {
            return true;
          }
          i = close + 1;
          continue;
        }
        if (i + 1 < raw.size() && isNameStart(raw[i + 1])) {
          std::size_t end = i + 2;
          while (end < raw.size() && isNameChar(raw[end])) {
            ++end;
          }
          const std::string name(raw.substr(i + 1, end - (i + 1)));
          const char* value = std::getenv(name.c_str());
          if (value == nullptr || value[0] == '\0') {
            return true;
          }
          i = end;
          continue;
        }
        ++i;
      }
      return false;
    };

    const auto resolveExpanded = [&](std::string_view raw) {
      if (raw.empty() || containsUndefinedVariable(raw)) {
        return std::filesystem::path{};
      }
      const std::filesystem::path resolved = expandUserPath(expandEnvVars(raw));
      return resolved.is_absolute() ? resolved : std::filesystem::path{};
    };

    if (const char* xdgPictures = std::getenv("XDG_PICTURES_DIR"); xdgPictures != nullptr && xdgPictures[0] != '\0') {
      if (const std::filesystem::path resolved = resolveExpanded(xdgPictures); !resolved.empty()) {
        return resolved;
      }
    }

    const char* home = std::getenv("HOME");
    if (home == nullptr || home[0] == '\0') {
      return std::filesystem::path("/tmp");
    }
    const std::filesystem::path homePath(home);
    if (!homePath.is_absolute()) {
      return std::filesystem::path("/tmp");
    }

    std::filesystem::path configHome = homePath / ".config";
    if (const char* xdgConfig = std::getenv("XDG_CONFIG_HOME"); xdgConfig != nullptr && xdgConfig[0] != '\0') {
      const std::filesystem::path candidate(xdgConfig);
      if (candidate.is_absolute()) {
        configHome = candidate;
      }
    }

    std::ifstream userDirs(configHome / "user-dirs.dirs");
    std::string line;
    constexpr std::string_view key = "XDG_PICTURES_DIR";
    while (std::getline(userDirs, line)) {
      const std::size_t keyStart = line.find_first_not_of(" \t");
      if (keyStart == std::string::npos || line.compare(keyStart, key.size(), key) != 0) {
        continue;
      }
      const std::size_t equals = keyStart + key.size();
      if (equals >= line.size() || line[equals] != '=') {
        continue;
      }
      const std::size_t valueStart = line.find_first_not_of(" \t", equals + 1);
      if (valueStart == std::string::npos || line[valueStart] != '"') {
        continue;
      }

      std::string value;
      bool terminated = false;
      for (std::size_t i = valueStart + 1; i < line.size(); ++i) {
        if (line[i] == '"') {
          terminated = true;
          break;
        }
        if (line[i] == '\\' && i + 1 < line.size()) {
          ++i;
        }
        value.push_back(line[i]);
      }
      if (!terminated) {
        continue;
      }
      if (value.starts_with("$HOME/")) {
        return homePath / value.substr(6);
      }
      if (!value.empty() && value[0] == '/') {
        return std::filesystem::path(value);
      }
    }

    return homePath / "Pictures";
  }

  // Expand XDG base-directory tokens ($XDG_CONFIG_HOME, etc.) to absolute paths.
  [[nodiscard]] inline std::string expandXdgBaseDir(const std::string& path) {
    struct XdgBase {
      std::string_view token;
      std::string_view envVar;
      std::string_view homeDefault;
    };
    static constexpr std::array<XdgBase, 4> kBases = {{
        {"$XDG_CONFIG_HOME", "XDG_CONFIG_HOME", ".config"},
        {"$XDG_DATA_HOME", "XDG_DATA_HOME", ".local/share"},
        {"$XDG_STATE_HOME", "XDG_STATE_HOME", ".local/state"},
        {"$XDG_CACHE_HOME", "XDG_CACHE_HOME", ".cache"},
    }};
    for (const XdgBase& b : kBases) {
      if (!path.starts_with(b.token))
        continue;
      if (path.size() != b.token.size() && path[b.token.size()] != '/')
        continue;
      std::string base;
      if (const char* env = std::getenv(std::string(b.envVar).c_str()); env != nullptr && env[0] != '\0') {
        base = env;
      } else if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
        base = std::string(home) + "/" + std::string(b.homeDefault);
      } else {
        return path;
      }
      return base + path.substr(b.token.size());
    }
    return path;
  }

  [[nodiscard]] inline bool
  containsPath(const std::vector<std::filesystem::path>& paths, const std::filesystem::path& path) {
    return std::ranges::contains(paths, path);
  }

  [[nodiscard]] inline std::optional<std::string> readSmallTextFile(const std::filesystem::path& path) {
    std::ifstream file{path};
    if (!file.is_open()) {
      return std::nullopt;
    }

    std::string text;
    std::getline(file, text);
    if (text.empty()) {
      return std::nullopt;
    }

    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ' || text.back() == '\t')) {
      text.pop_back();
    }
    return text;
  }

  [[nodiscard]] inline std::string configDir() {
    const char* noctalia = (std::getenv("ANDROMEDA_CONFIG_HOME") ? std::getenv("ANDROMEDA_CONFIG_HOME") : std::getenv("NOCTALIA_CONFIG_HOME"));
    if (noctalia != nullptr && noctalia[0] != '\0') {
      return std::string(noctalia) + "/andromeda/shell";
    }
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    if (xdg != nullptr && xdg[0] != '\0') {
      return std::string(xdg) + "/andromeda/shell";
    }
    const char* home = std::getenv("HOME");
    if (home != nullptr && home[0] != '\0') {
      return std::string(home) + "/.config/andromeda/shell";
    }
    return {};
  }

  [[nodiscard]] inline std::string stateDir() {
    const char* noctalia = (std::getenv("ANDROMEDA_STATE_HOME") ? std::getenv("ANDROMEDA_STATE_HOME") : std::getenv("NOCTALIA_STATE_HOME"));
    if (noctalia != nullptr && noctalia[0] != '\0') {
      return std::string(noctalia) + "/andromeda/shell";
    }
    const char* xdg = std::getenv("XDG_STATE_HOME");
    if (xdg != nullptr && xdg[0] != '\0') {
      return std::string(xdg) + "/andromeda/shell";
    }
    const char* home = std::getenv("HOME");
    if (home != nullptr && home[0] != '\0') {
      return std::string(home) + "/.local/state/andromeda/shell";
    }
    return {};
  }

  [[nodiscard]] inline std::string dataDir() {
    const char* noctalia = (std::getenv("ANDROMEDA_DATA_HOME") ? std::getenv("ANDROMEDA_DATA_HOME") : std::getenv("NOCTALIA_DATA_HOME"));
    if (noctalia != nullptr && noctalia[0] != '\0') {
      return std::string(noctalia) + "/andromeda/shell";
    }
    const char* xdg = std::getenv("XDG_DATA_HOME");
    if (xdg != nullptr && xdg[0] != '\0') {
      return std::string(xdg) + "/andromeda/shell";
    }
    const char* home = std::getenv("HOME");
    if (home != nullptr && home[0] != '\0') {
      return std::string(home) + "/.local/share/andromeda/shell";
    }
    return {};
  }

  // Git-source repo caches. Host-managed, re-fetchable, so they live under the
  // state dir — never config.
  [[nodiscard]] inline std::string pluginSourcesDir() {
    const std::string base = stateDir();
    if (base.empty()) {
      return {};
    }
    return base + "/plugins/sources";
  }

  // Exported runtime files for enabled git-source plugins. Re-derivable from
  // source repos; path sources and local dev plugins do not use this directory.
  [[nodiscard]] inline std::string pluginMaterializedDir() {
    const std::string base = stateDir();
    if (base.empty()) {
      return {};
    }
    return base + "/plugins/materialized";
  }

  // Persistent per-plugin data directory. Survives plugin updates (unlike the
  // materialized runtime dir) and honors the ANDROMEDA_STATE_HOME override.
  // Caller is responsible for creating it. Empty if no state home resolves.
  [[nodiscard]] inline std::string pluginDataDir(const std::string& pluginId) {
    const std::string base = stateDir();
    if (base.empty() || pluginId.empty()) {
      return {};
    }
    return base + "/plugins/data/" + pluginId;
  }

  [[nodiscard]] inline std::vector<std::uint8_t> readBinaryFile(const std::string& path) {
    if (path.empty()) {
      return {};
    }

    std::error_code ec;
    const std::filesystem::path fsPath = expandUserPath(path);
    if (!std::filesystem::is_regular_file(fsPath, ec) || ec) {
      return {};
    }

    const std::uintmax_t fileSize = std::filesystem::file_size(fsPath, ec);
    if (ec || fileSize == 0) {
      return {};
    }

    constexpr std::uintmax_t kMaxBinaryReadBytes = 256ULL * 1024ULL * 1024ULL;
    if (fileSize > kMaxBinaryReadBytes) {
      return {};
    }

    std::ifstream file(fsPath, std::ios::binary);
    if (!file) {
      return {};
    }

    std::vector<std::uint8_t> data(static_cast<std::size_t>(fileSize));
    file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(fileSize));
    if (!file) {
      return {};
    }
    return data;
  }

  // Expands ~ and resolves relative paths against baseDir when provided; otherwise uses the
  // process working directory for relative paths.
  [[nodiscard]] inline std::filesystem::path
  resolvePath(std::string_view path, std::optional<std::string_view> baseDir = std::nullopt) {
    if (path.empty() || path.starts_with("color:")) {
      return std::filesystem::path(path);
    }

    std::filesystem::path resolved = expandUserPath(std::string(path));
    if (!resolved.is_absolute()) {
      if (baseDir.has_value() && !baseDir->empty()) {
        resolved = std::filesystem::path(*baseDir) / resolved;
      } else {
        std::error_code ec;
        resolved = std::filesystem::absolute(resolved, ec);
        if (ec) {
          return std::filesystem::path(path);
        }
      }
    }

    std::error_code ec;
    return resolved.lexically_normal();
  }

  [[nodiscard]] inline std::string normalizeWallpaperPath(std::string_view path) {
    if (path.empty() || path.starts_with("color:")) {
      return std::string(path);
    }

    std::error_code ec;
    auto absolute = std::filesystem::absolute(std::filesystem::path(path), ec);
    if (ec) {
      absolute = std::filesystem::path(path);
    }
    return expandUserPath(absolute.lexically_normal().string()).string();
  }

  // Walks `root` recursively, following directory symlinks. follow_directory_symlink has no cycle detection, so each
  // real directory (by canonical path) is entered at most once: a link back up the tree or a second link to an
  // already walked directory is skipped. `onDirectory(entry)` runs for each newly reached directory and returns
  // whether to descend into it; `onFile(entry)` runs for every other entry. The root is claimed but not reported.
  // Unreadable directories are skipped; any other iteration error ends the walk.
  template <typename OnDirectory, typename OnFile>
  void walkDirectoryTree(const std::filesystem::path& root, OnDirectory&& onDirectory, OnFile&& onFile) {
    std::unordered_set<std::string> claimed;
    const auto claim = [&claimed](const std::filesystem::path& dir) {
      std::error_code ec;
      const auto resolved = std::filesystem::canonical(dir, ec);
      return !ec && claimed.insert(resolved.string()).second;
    };
    if (!claim(root)) {
      return;
    }

    constexpr auto options = std::filesystem::directory_options::skip_permission_denied
        | std::filesystem::directory_options::follow_directory_symlink;
    std::error_code ec;
    for (std::filesystem::recursive_directory_iterator it(root, options, ec), end; !ec && it != end; it.increment(ec)) {
      std::error_code typeEc;
      if (it->is_directory(typeEc) && !typeEc) {
        if (!claim(it->path()) || !onDirectory(*it)) {
          it.disable_recursion_pending();
        }
        continue;
      }
      onFile(*it);
    }
  }

} // namespace FileUtils
