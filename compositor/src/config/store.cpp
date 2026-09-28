#include "config/store.h"

#include "config/fields.h"
#include "config/keybind_parse.h"
#include "config/managed_settings.h"
#include "core/log.h"
#include "umbriel_data_dir.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

namespace umbriel {

  namespace {

    constexpr Logger kLog("config");

    std::filesystem::path userConfigPath() {
      if (const char* xdgConfigHome = std::getenv("XDG_CONFIG_HOME");
          xdgConfigHome != nullptr && xdgConfigHome[0] != '\0') {
        return std::filesystem::path(xdgConfigHome) / "umbriel/config.toml";
      }
      if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
        return std::filesystem::path(home) / ".config/umbriel/config.toml";
      }
      return std::filesystem::path(".config/umbriel/config.toml");
    }

    enum class ConfigPathKind {
      Missing,
      RegularFile,
      Unavailable,
    };

    struct ConfigPathProbe {
      ConfigPathKind kind = ConfigPathKind::Missing;
      std::error_code error;
    };

    ConfigPathProbe probeConfigPath(const std::filesystem::path& path) {
      std::error_code error;
      const std::filesystem::file_status status = std::filesystem::status(path, error);
      if (error) {
        if (error == std::errc::no_such_file_or_directory || error == std::errc::not_a_directory) {
          return {};
        }
        return {.kind = ConfigPathKind::Unavailable, .error = error};
      }
      if (status.type() == std::filesystem::file_type::not_found) {
        return {};
      }
      return {
          .kind = std::filesystem::is_regular_file(status) ? ConfigPathKind::RegularFile : ConfigPathKind::Unavailable,
          .error = {},
      };
    }

    std::vector<std::filesystem::path> defaultConfigCandidates() {
      std::vector<std::filesystem::path> candidates{userConfigPath()};
      const char* configuredDirs = std::getenv("XDG_CONFIG_DIRS");
      const std::string_view configDirs = configuredDirs != nullptr && configuredDirs[0] != '\0'
          ? std::string_view(configuredDirs)
          : std::string_view("/etc/xdg");
      size_t offset = 0;
      while (offset <= configDirs.size()) {
        const size_t separator = configDirs.find(':', offset);
        const std::string_view directory = configDirs.substr(offset, separator - offset);
        if (!directory.empty()) {
          candidates.push_back(std::filesystem::path(directory) / "umbriel/config.toml");
        }
        if (separator == std::string_view::npos) {
          break;
        }
        offset = separator + 1;
      }

      candidates.push_back(std::filesystem::path(kDataDir) / "umbriel/config.toml");
      return candidates;
    }

    struct ConfigSelection {
      std::filesystem::path root;
      std::vector<std::filesystem::path> watchPaths;
      bool found = false;
    };

    ConfigSelection selectDefaultConfig(const std::vector<std::filesystem::path>& candidates) {
      ConfigSelection selection{};
      selection.watchPaths = candidates;
      for (const std::filesystem::path& candidate : candidates) {
        if (probeConfigPath(candidate).kind != ConfigPathKind::Missing) {
          selection.root = candidate;
          selection.found = true;
          return selection;
        }
      }
      selection.root = candidates.empty() ? userConfigPath() : candidates.front();
      if (selection.watchPaths.empty()) {
        selection.watchPaths.push_back(selection.root);
      }
      return selection;
    }

    // input.toml was the settings file before every setting shared one; take it over when it is still ours.
    void adoptLegacyInputSettings(const std::filesystem::path& configRoot) {
      const std::filesystem::path legacy = configRoot.parent_path() / "input.toml";
      const std::filesystem::path current = managedSettingsFile(configRoot);
      std::error_code error;
      if (std::filesystem::exists(current, error) || !std::filesystem::exists(legacy, error)) {
        return;
      }
      std::ifstream in(legacy);
      std::string firstLine;
      std::getline(in, firstLine);
      if (!firstLine.starts_with("# Written by Umbriel")) {
        return;
      }
      in.close();
      std::filesystem::rename(legacy, current, error);
      if (error) {
        kLog.warn("config: cannot move {} to {}: {}", legacy.string(), current.string(), error.message());
      }
    }

    // Reset the store for a load of `rootPath`, then read it if it is a file.
    ConfigParseOutcome parseInto(
        Config& out, const std::filesystem::path& rootPath, const std::vector<std::filesystem::path>& watchPaths
    ) {
      configStore().beginLoad(watchPaths);
      const std::filesystem::path settingsFile = managedSettingsFile(rootPath);
      configStore().addWatchPath(settingsFile);
      const ConfigPathProbe root = probeConfigPath(rootPath);
      std::error_code settingsError;
      if (root.kind == ConfigPathKind::Missing && !std::filesystem::exists(settingsFile, settingsError)) {
        return ConfigParseOutcome::Missing;
      }
      if (root.kind == ConfigPathKind::Unavailable) {
        const std::string reason = root.error ? root.error.message() : "not a regular file";
        emitDiag(
            ConfigDiagnostic::Severity::Error, nullptr,
            std::format("cannot inspect config file {}: {}", rootPath.string(), reason)
        );
        return ConfigParseOutcome::Fatal;
      }
      return parseConfig(out, rootPath);
    }

  } // namespace

  void ConfigStore::beginLoad(const std::vector<std::filesystem::path>& watchPaths) {
    m_diagnostics.clear();
    m_missingIncludes = false;
    m_watchPaths.clear();
    for (const std::filesystem::path& path : watchPaths) {
      addWatchPath(path);
    }
  }

  void ConfigStore::addDiagnostic(ConfigDiagnostic diagnostic) { m_diagnostics.push_back(std::move(diagnostic)); }

  void ConfigStore::addWatchPath(std::filesystem::path path) {
    if (std::ranges::find(m_watchPaths, path) == m_watchPaths.end()) {
      m_watchPaths.push_back(std::move(path));
    }
  }

  void ConfigStore::sortDiagnostics() {
    // Stable so two diagnostics on the same key keep the order they were found
    // in. File-less entries (whole-config errors) sort first.
    std::ranges::stable_sort(m_diagnostics, [](const ConfigDiagnostic& a, const ConfigDiagnostic& b) {
      return std::tie(a.file, a.line, a.column) < std::tie(b.file, b.line, b.column);
    });
  }

  ConfigReloadResult ConfigStore::commit(Config&& config, std::filesystem::path rootPath, bool fileMissing) {
    // Computed before the move, and only after the first load: everything is new
    // the first time through.
    ConfigReloadResult result{
        .success = true,
        .change = m_generation == 0 ? ConfigChange::everything() : ConfigChange::between(m_config, config),
        .effects = m_generation == 0 ? ConfigEffects::everything() : ConfigEffects::between(m_config, config),
    };
    m_config = std::move(config);
    m_rootPath = std::move(rootPath);
    m_fileMissing = fileMissing;
    ++m_generation;
    return result;
  }

  void ConfigStore::setRootPath(std::filesystem::path path, bool explicitPath) {
    m_rootPath = std::move(path);
    m_explicitPath = explicitPath;
  }

  ConfigStore& configStore() {
    static ConfigStore store;
    return store;
  }

  const Config& config() { return configStore().config(); }

  const std::vector<ConfigDiagnostic>& configDiagnostics() { return configStore().diagnostics(); }

  const std::filesystem::path& configRootPath() { return configStore().rootPath(); }

  bool configFileMissing() { return configStore().fileMissing(); }

  bool configHasMissingIncludes() { return configStore().missingIncludes(); }

  bool ConfigStore::load(const char* explicitPath) {
    ConfigSelection selection;
    if (explicitPath != nullptr) {
      m_implicitCandidates.clear();
      selection.root = std::filesystem::path(explicitPath);
      selection.watchPaths.push_back(selection.root);
    } else {
      m_implicitCandidates = defaultConfigCandidates();
      selection = selectDefaultConfig(m_implicitCandidates);
    }
    setRootPath(selection.root, explicitPath != nullptr);
    adoptLegacyInputSettings(selection.root);

    Config loaded;
    loaded.keybinds = defaultKeybinds();
    const ConfigParseOutcome outcome = parseInto(loaded, selection.root, selection.watchPaths);
    const bool missing = outcome == ConfigParseOutcome::Missing;
    if (missing) {
      if (m_explicitPath) {
        emitDiag(
            ConfigDiagnostic::Severity::Error, nullptr,
            std::format("config file not found: {}", selection.root.string())
        );
      } else {
        kLog.info("no config file found: {}, using defaults", selection.root.string());
      }
    }
    sortDiagnostics();
    if (outcome == ConfigParseOutcome::Fatal || (missing && m_explicitPath)) {
      return false;
    }
    (void)commit(std::move(loaded), selection.root, missing);
    return true;
  }

  ConfigReloadResult ConfigStore::reload() {
    ConfigSelection selection;
    if (m_explicitPath) {
      selection.root = m_rootPath;
      selection.watchPaths.push_back(selection.root);
    } else {
      selection = selectDefaultConfig(m_implicitCandidates);
    }

    Config loaded;
    loaded.keybinds = defaultKeybinds();
    const ConfigParseOutcome outcome = parseInto(loaded, selection.root, selection.watchPaths);
    const bool missing = outcome == ConfigParseOutcome::Missing;
    if (m_explicitPath && missing) {
      emitDiag(
          ConfigDiagnostic::Severity::Error, nullptr, std::format("config file not found: {}", selection.root.string())
      );
    }
    sortDiagnostics();
    if (outcome != ConfigParseOutcome::Loaded) {
      if (!m_explicitPath && !selection.found && missing) {
        kLog.info("no config file found: {}, using defaults", selection.root.string());
        return commit(std::move(loaded), selection.root, true);
      }
      kLog.warn("config reload failed; keeping previous configuration");
      return {};
    }
    return commit(std::move(loaded), selection.root, false);
  }

  bool loadConfig(const char* explicitPath) { return configStore().load(explicitPath); }

  ConfigReloadResult reloadConfig() { return configStore().reload(); }

  const std::vector<std::filesystem::path>& configWatchPaths() { return configStore().watchPaths(); }

} // namespace umbriel
